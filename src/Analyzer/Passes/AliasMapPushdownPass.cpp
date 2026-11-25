#include <Analyzer/Passes/AliasMapPushdownPass.h>

#include <Analyzer/AggregationUtils.h>
#include <Analyzer/ColumnNode.h>
#include <Analyzer/ConstantNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/InDepthQueryTreeVisitor.h>
#include <Analyzer/LambdaNode.h>
#include <Analyzer/TableNode.h>
#include <Analyzer/Utils.h>
#include <Columns/ColumnConst.h>
#include <Columns/validateColumnType.h>
#include <Common/SipHash.h>
#include <Common/UnorderedSetWithMemoryTracking.h>
#include <Common/VectorWithMemoryTracking.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/IDataType.h>
#include <Functions/FunctionFactory.h>
#include <Interpreters/Context.h>
#include <Storages/IStorage.h>

#include <fmt/format.h>

namespace DB
{

namespace Setting
{
    extern const SettingsBool optimize_map_alias_pushdown;
}

namespace
{

constexpr size_t MAX_ALIAS_MAP_PUSHDOWN_DEPTH = 16;

enum class MapAccessKind : uint8_t
{
    ArrayElement,
    ContainsKey,
};

struct PushdownContext
{
    const ContextPtr & query_context;
    const TableNode * root_table_node = nullptr;
    bool applied_rule = false;
};

std::optional<bool> tryGetConstBool(const QueryTreeNodePtr & node)
{
    const auto * const_node = node ? node->as<ConstantNode>() : nullptr;
    if (!const_node || const_node->isNull())
        return std::nullopt;

    const auto & val = const_node->getValue();
    if (val.getType() == Field::Types::UInt64)
        return val.safeGet<UInt64>() != 0;
    if (val.getType() == Field::Types::Int64)
        return val.safeGet<Int64>() != 0;
    return std::nullopt;
}

QueryTreeNodePtr makeConstUInt8(bool value)
{
    return std::make_shared<ConstantNode>(Field(static_cast<UInt64>(value ? 1 : 0)), std::make_shared<DataTypeUInt8>());
}

QueryTreeNodePtr makeMapDefaultConstant(const DataTypePtr & value_type)
{
    return std::make_shared<ConstantNode>(value_type->getDefault(), value_type);
}

bool isDefaultConstantForType(const QueryTreeNodePtr & node, const DataTypePtr & value_type)
{
    const auto * const_node = node ? node->as<ConstantNode>() : nullptr;
    if (!const_node)
        return false;
    return const_node->getResultType()->equals(*value_type) && const_node->getValue() == value_type->getDefault();
}

bool referencesLambdaParam(const QueryTreeNodePtr & node, const IQueryTreeNode * lambda_args_ptr, const String & param_name)
{
    if (!node || isQueryOrUnionNode(node))
        return false;

    if (const auto * col_node = node->as<ColumnNode>())
    {
        auto src = col_node->getColumnSourceOrNull();
        if (src && src.get() == lambda_args_ptr && col_node->getColumnName() == param_name)
            return true;
    }

    for (const auto & child : node->getChildren())
    {
        if (child && referencesLambdaParam(child, lambda_args_ptr, param_name))
            return true;
    }
    return false;
}

bool isLambdaParamNode(const QueryTreeNodePtr & node, const IQueryTreeNode * lambda_args_ptr, const String & param_name)
{
    const auto * col_node = node ? node->as<ColumnNode>() : nullptr;
    if (!col_node)
        return false;
    auto src = col_node->getColumnSourceOrNull();
    return src && src.get() == lambda_args_ptr && col_node->getColumnName() == param_name;
}

bool hasDisallowedRewriteBodyNodes(const QueryTreeNodePtr & node)
{
    if (!node)
        return false;
    if (isQueryOrUnionNode(node))
        return true;

    if (const auto * fn = node->as<FunctionNode>())
    {
        if (!fn->isOrdinaryCall() || fn->getFunctionName() == "arrayJoin")
            return true;
    }

    for (const auto & child : node->getChildren())
    {
        if (child && hasDisallowedRewriteBodyNodes(child))
            return true;
    }
    return false;
}

std::optional<bool> tryEvaluateConstantInTuple(const ConstantNode & lhs, const ConstantNode & rhs, bool is_not_in)
{
    if (lhs.isNull() || rhs.isNull())
        return std::nullopt;

    const Field & lhs_val = lhs.getValue();
    const Field & rhs_val = rhs.getValue();

    auto match_scalar = [&](const Field & elem) -> std::optional<bool>
    {
        if (elem.isNull())
            return std::nullopt;
        if (lhs_val.getType() != elem.getType())
            return std::nullopt;
        return lhs_val == elem;
    };

    bool found = false;
    if (rhs_val.getType() == Field::Types::Tuple)
    {
        const auto & tuple = rhs_val.safeGet<Tuple>();
        for (const auto & elem : tuple)
        {
            auto matched = match_scalar(elem);
            if (!matched)
                return std::nullopt;
            if (*matched)
            {
                found = true;
                break;
            }
        }
    }
    else
    {
        auto matched = match_scalar(rhs_val);
        if (!matched)
            return std::nullopt;
        found = *matched;
    }

    return is_not_in ? !found : found;
}

QueryTreeNodePtr tryFoldResolvedFunction(const QueryTreeNodePtr & node)
{
    const auto * fn = node ? node->as<FunctionNode>() : nullptr;
    if (!fn || !fn->isResolved() || !fn->isOrdinaryCall())
        return node;

    const String & name = fn->getFunctionName();
    const auto & args = fn->getArguments().getNodes();

    if (name == "in" || name == "notIn")
    {
        if (args.size() == 2 && !fn->getResultType()->isNullable())
        {
            const auto * lhs_const = args[0]->as<ConstantNode>();
            const auto * rhs_const = args[1]->as<ConstantNode>();
            if (lhs_const && rhs_const)
            {
                if (auto res = tryEvaluateConstantInTuple(*lhs_const, *rhs_const, name == "notIn"))
                    return makeConstUInt8(*res);
            }
        }
        return node;
    }

    auto func_base = fn->getFunction();
    if (!func_base || !func_base->isSuitableForConstantFolding() || !func_base->isDeterministicInScopeOfQuery())
        return node;

    auto arg_columns = fn->getArgumentColumns();
    for (const auto & arg : arg_columns)
    {
        if (!arg.column || !isColumnConst(*arg.column))
            return node;
    }

    auto result_type = func_base->getResultType();
    auto executable_func = func_base->prepare(arg_columns);
    auto col = executable_func->execute(arg_columns, result_type, 1, /*dry_run=*/ true);
    if (col && col->empty() && isColumnConst(*col))
        col = col->cloneResized(1);

    const auto * col_const = col ? typeid_cast<const ColumnConst *>(col.get()) : nullptr;
    if (!col_const || col_const->getDataColumn().isDummy())
        return node;
    if (!columnMatchesType(*col, *result_type))
        return node;
    if (col->byteSize() >= 1_MiB)
        return node;

    bool all_deterministic = true;
    for (const auto & arg : args)
    {
        if (const auto * arg_const = arg->as<ConstantNode>())
            all_deterministic &= arg_const->isDeterministic();
    }
    const bool is_deterministic = all_deterministic && func_base->isDeterministic();

    return std::make_shared<ConstantNode>(
        ConstantValue{col_const->getPtr(), std::move(result_type)},
        node,
        is_deterministic);
}

QueryTreeNodePtr simplifyLocalNode(const QueryTreeNodePtr & node, const ContextPtr & context)
{
    auto * fn = node ? node->as<FunctionNode>() : nullptr;
    if (!fn || !fn->isResolved() || !fn->isOrdinaryCall())
        return node;

    const String & name = fn->getFunctionName();
    auto & args = fn->getArguments().getNodes();

    if (name == "and" && !fn->getResultType()->isNullable())
    {
        QueryTreeNodes kept;
        kept.reserve(args.size());
        for (const auto & arg : args)
        {
            if (auto b = tryGetConstBool(arg))
            {
                if (!*b)
                    return makeConstUInt8(false);
                continue;
            }
            kept.push_back(arg);
        }
        if (kept.empty())
            return makeConstUInt8(true);
        if (kept.size() == 1)
        {
            if (kept[0]->getResultType()->equals(*fn->getResultType()))
                return kept[0];
            return buildCastFunction(kept[0], fn->getResultType(), context, true);
        }
        if (kept.size() != args.size())
            return tryFoldResolvedFunction(createResolvedFunction(context, "and", std::move(kept)));
    }
    else if (name == "or" && !fn->getResultType()->isNullable())
    {
        QueryTreeNodes kept;
        kept.reserve(args.size());
        for (const auto & arg : args)
        {
            if (auto b = tryGetConstBool(arg))
            {
                if (*b)
                    return makeConstUInt8(true);
                continue;
            }
            kept.push_back(arg);
        }
        if (kept.empty())
            return makeConstUInt8(false);
        if (kept.size() == 1)
        {
            if (kept[0]->getResultType()->equals(*fn->getResultType()))
                return kept[0];
            return buildCastFunction(kept[0], fn->getResultType(), context, true);
        }
        if (kept.size() != args.size())
            return tryFoldResolvedFunction(createResolvedFunction(context, "or", std::move(kept)));
    }
    else if (name == "if" && args.size() == 3)
    {
        if (auto b = tryGetConstBool(args[0]))
        {
            QueryTreeNodePtr chosen = *b ? args[1] : args[2];
            if (!chosen->getResultType()->equals(*fn->getResultType()))
                chosen = tryFoldResolvedFunction(buildCastFunction(chosen, fn->getResultType(), context, true));
            return chosen;
        }
    }
    else if (name == "multiIf" && args.size() >= 3 && (args.size() % 2 == 1))
    {
        QueryTreeNodes remaining_pairs;
        for (size_t i = 0; i + 1 < args.size(); i += 2)
        {
            auto b = tryGetConstBool(args[i]);
            if (b && !*b)
                continue;
            if (b && *b)
            {
                if (remaining_pairs.empty())
                {
                    QueryTreeNodePtr chosen = args[i + 1];
                    if (!chosen->getResultType()->equals(*fn->getResultType()))
                        chosen = tryFoldResolvedFunction(buildCastFunction(chosen, fn->getResultType(), context, true));
                    return chosen;
                }
                remaining_pairs.push_back(args[i + 1]);
                QueryTreeNodePtr rebuilt = createResolvedFunction(context, "multiIf", std::move(remaining_pairs));
                if (!rebuilt->getResultType()->equals(*fn->getResultType()))
                    rebuilt = buildCastFunction(rebuilt, fn->getResultType(), context, true);
                return tryFoldResolvedFunction(rebuilt);
            }
            remaining_pairs.push_back(args[i]);
            remaining_pairs.push_back(args[i + 1]);
        }

        if (remaining_pairs.empty())
        {
            QueryTreeNodePtr chosen = args.back();
            if (!chosen->getResultType()->equals(*fn->getResultType()))
                chosen = tryFoldResolvedFunction(buildCastFunction(chosen, fn->getResultType(), context, true));
            return chosen;
        }

        if (remaining_pairs.size() != args.size() - 1)
        {
            remaining_pairs.push_back(args.back());
            const String rebuilt_name = (remaining_pairs.size() == 3) ? "if" : "multiIf";
            QueryTreeNodePtr rebuilt = createResolvedFunction(context, rebuilt_name, std::move(remaining_pairs));
            if (!rebuilt->getResultType()->equals(*fn->getResultType()))
                rebuilt = buildCastFunction(rebuilt, fn->getResultType(), context, true);
            return tryFoldResolvedFunction(rebuilt);
        }
    }

    return tryFoldResolvedFunction(node);
}

QueryTreeNodePtr substituteAndFoldExpr(
    const QueryTreeNodePtr & node,
    const IQueryTreeNode * k_source,
    const String & k_name,
    const QueryTreeNodePtr & k_replacement,
    const IQueryTreeNode * v_source,
    const String & v_name,
    const QueryTreeNodePtr & v_replacement,
    const ContextPtr & context)
{
    if (!node || isQueryOrUnionNode(node))
        return node ? node->clone() : nullptr;

    if (const auto * col_node = node->as<ColumnNode>())
    {
        auto src = col_node->getColumnSourceOrNull();
        auto make_cast_if_needed = [&](QueryTreeNodePtr rep)
        {
            if (rep && !rep->getResultType()->equals(*col_node->getColumnType()))
                rep = tryFoldResolvedFunction(buildCastFunction(rep, col_node->getColumnType(), context, true));
            return rep;
        };
        if (k_replacement && src && src.get() == k_source && col_node->getColumnName() == k_name)
            return make_cast_if_needed(k_replacement->clone());
        if (v_replacement && src && src.get() == v_source && col_node->getColumnName() == v_name)
            return make_cast_if_needed(v_replacement->clone());
        return node->clone();
    }

    QueryTreeNodePtr cloned = node->clone();
    if (auto * inner_lambda = cloned->as<LambdaNode>())
    {
        if (inner_lambda->getExpression())
        {
            const auto orig_type = inner_lambda->getExpression()->getResultType();
            QueryTreeNodePtr new_expr = substituteAndFoldExpr(
                inner_lambda->getExpression(), k_source, k_name, k_replacement, v_source, v_name, v_replacement, context);
            if (orig_type && new_expr && !new_expr->getResultType()->equals(*orig_type))
                new_expr = tryFoldResolvedFunction(buildCastFunction(new_expr, orig_type, context, true));
            inner_lambda->getExpression() = std::move(new_expr);
        }
        return cloned;
    }

    if (auto * fn = cloned->as<FunctionNode>())
    {
        auto & args = fn->getArguments().getNodes();
        bool has_lambda_arg = false;
        for (auto & arg : args)
        {
            if (arg && arg->getNodeType() == QueryTreeNodeType::LAMBDA)
                has_lambda_arg = true;
            arg = substituteAndFoldExpr(
                arg, k_source, k_name, k_replacement, v_source, v_name, v_replacement, context);
        }

        if (fn->isResolved() && fn->isOrdinaryCall() && !has_lambda_arg)
        {
            if (fn->getFunctionName() != "in" && fn->getFunctionName() != "notIn")
            {
                auto fn_name = fn->getFunctionName();
                resolveOrdinaryFunctionNodeByName(*fn, fn_name, context);
            }
            return simplifyLocalNode(cloned, context);
        }
    }

    return cloned;
}

QueryTreeNodePtr localFoldOnly(const QueryTreeNodePtr & expr, const ContextPtr & context)
{
    return substituteAndFoldExpr(expr, nullptr, {}, nullptr, nullptr, {}, nullptr, context);
}

DataTypePtr getMapValueTypeOrNull(const QueryTreeNodePtr & map_expr)
{
    if (!map_expr)
        return nullptr;
    auto type = map_expr->getResultType();
    if (!type || !isMap(type))
        return nullptr;
    return assert_cast<const DataTypeMap &>(*type).getValueType();
}

DataTypePtr getMapKeyTypeOrNull(const QueryTreeNodePtr & map_expr)
{
    if (!map_expr)
        return nullptr;
    auto type = map_expr->getResultType();
    if (!type || !isMap(type))
        return nullptr;
    return assert_cast<const DataTypeMap &>(*type).getKeyType();
}

QueryTreeNodePtr buildBaseMapAccess(
    const QueryTreeNodePtr & map_expr,
    const QueryTreeNodePtr & key_expr,
    MapAccessKind kind,
    const ContextPtr & context)
{
    QueryTreeNodePtr key_arg = key_expr->clone();
    if (auto map_key_type = getMapKeyTypeOrNull(map_expr))
    {
        if (!key_arg->getResultType()->equals(*map_key_type))
            key_arg = tryFoldResolvedFunction(buildCastFunction(key_arg, map_key_type, context, true));
    }

    const String fn_name = (kind == MapAccessKind::ArrayElement) ? "arrayElement" : "mapContainsKey";
    return createResolvedFunction(context, fn_name, {map_expr->clone(), std::move(key_arg)});
}

QueryTreeNodePtr tryMatchPrefixExtract(
    const QueryTreeNodePtr & out_k,
    const IQueryTreeNode * k_source,
    const String & k_name,
    const QueryTreeNodePtr & inner_map,
    String & out_prefix)
{
    const auto * sub_fn = out_k ? out_k->as<FunctionNode>() : nullptr;
    if (!sub_fn || !sub_fn->isOrdinaryCall())
        return nullptr;

    const String & sub_name = sub_fn->getFunctionName();
    if (sub_name != "substring" && sub_name != "substr" && sub_name != "substringUTF8")
        return nullptr;

    const auto & sub_args = sub_fn->getArguments().getNodes();
    if (sub_args.size() != 2 || !isLambdaParamNode(sub_args[0], k_source, k_name))
        return nullptr;

    const auto * offset_const = sub_args[1]->as<ConstantNode>();
    if (!offset_const || offset_const->isNull())
        return nullptr;

    Int64 offset_val = 0;
    const auto & offset_field = offset_const->getValue();
    if (offset_field.getType() == Field::Types::UInt64)
        offset_val = static_cast<Int64>(offset_field.safeGet<UInt64>());
    else if (offset_field.getType() == Field::Types::Int64)
        offset_val = offset_field.safeGet<Int64>();
    else
        return nullptr;

    const auto * filter_fn = inner_map ? inner_map->as<FunctionNode>() : nullptr;
    if (!filter_fn || filter_fn->getFunctionName() != "mapFilter")
        return nullptr;

    const auto & filter_args = filter_fn->getArguments().getNodes();
    if (filter_args.size() != 2)
        return nullptr;

    const auto * filter_lambda = filter_args[0]->as<LambdaNode>();
    if (!filter_lambda || filter_lambda->getArguments().getNames().size() != 2)
        return nullptr;

    const auto * sw_fn = filter_lambda->getExpression()->as<FunctionNode>();
    if (!sw_fn || sw_fn->getFunctionName() != "startsWith")
        return nullptr;

    const auto & sw_args = sw_fn->getArguments().getNodes();
    if (sw_args.size() != 2)
        return nullptr;

    const auto * f_lambda_args = &filter_lambda->getArguments();
    const String & fk_name = filter_lambda->getArguments().getNames()[0];
    if (!isLambdaParamNode(sw_args[0], f_lambda_args, fk_name))
        return nullptr;

    const auto * prefix_const = sw_args[1]->as<ConstantNode>();
    if (!prefix_const || prefix_const->isNull() || prefix_const->getValue().getType() != Field::Types::String)
        return nullptr;

    const String & prefix = prefix_const->getValue().safeGet<String>();
    if (prefix.empty() || offset_val != static_cast<Int64>(prefix.size() + 1))
        return nullptr;

    for (unsigned char ch : prefix)
    {
        if (ch >= 0x80)
            return nullptr;
    }

    out_prefix = prefix;
    return filter_args[1];
}

QueryTreeNodePtr tryPushMapAccess(
    const QueryTreeNodePtr & map_expr,
    const QueryTreeNodePtr & key_expr,
    MapAccessKind kind,
    PushdownContext & ctx,
    size_t depth)
{
    if (!map_expr || !key_expr || depth >= MAX_ALIAS_MAP_PUSHDOWN_DEPTH)
        return nullptr;

    auto value_type = getMapValueTypeOrNull(map_expr);
    if (!value_type)
        return nullptr;

    /// Rule 1: Nested ALIAS ColumnNode on the same TableNode.
    if (const auto * col_node = map_expr->as<ColumnNode>())
    {
        if (col_node->hasExpression())
        {
            auto src = col_node->getColumnSourceOrNull();
            if (src && src.get() == ctx.root_table_node)
            {
                ctx.applied_rule = true;
                return tryPushMapAccess(col_node->getExpression(), key_expr, kind, ctx, depth + 1);
            }
        }
        return buildBaseMapAccess(map_expr, key_expr, kind, ctx.query_context);
    }

    const auto * fn = map_expr->as<FunctionNode>();
    if (!fn || !fn->isResolved() || !fn->isOrdinaryCall())
        return buildBaseMapAccess(map_expr, key_expr, kind, ctx.query_context);

    const String & fn_name = fn->getFunctionName();
    const auto & args = fn->getArguments().getNodes();

    /// Unwrap transparent _CAST(inner_map, 'Map(...)') inserted for declared ALIAS column type alignment.
    if (fn_name == "_CAST" && args.size() == 2 && isMap(args[0]->getResultType()))
    {
        auto inner_res = tryPushMapAccess(args[0], key_expr, kind, ctx, depth + 1);
        if (!inner_res)
            return nullptr;
        if (kind == MapAccessKind::ArrayElement && !inner_res->getResultType()->equals(*value_type))
            inner_res = tryFoldResolvedFunction(buildCastFunction(inner_res, value_type, ctx.query_context, true));
        return inner_res;
    }

    /// Rule 2: Literal `map(k1, v1, k2, v2, ...)`
    if (fn_name == "map" && (args.size() % 2 == 0))
    {
        ctx.applied_rule = true;
        if (args.empty())
            return (kind == MapAccessKind::ContainsKey) ? makeConstUInt8(false) : makeMapDefaultConstant(value_type);

        if (kind == MapAccessKind::ContainsKey)
        {
            QueryTreeNodes eq_nodes;
            eq_nodes.reserve(args.size() / 2);
            for (size_t i = 0; i < args.size(); i += 2)
                eq_nodes.push_back(createResolvedFunction(ctx.query_context, "equals", {key_expr->clone(), args[i]->clone()}));

            QueryTreeNodePtr result = (eq_nodes.size() == 1)
                ? eq_nodes[0]
                : createResolvedFunction(ctx.query_context, "or", std::move(eq_nodes));
            return localFoldOnly(result, ctx.query_context);
        }

        QueryTreeNodes multi_if_args;
        multi_if_args.reserve(args.size() + 1);
        for (size_t i = 0; i < args.size(); i += 2)
        {
            multi_if_args.push_back(createResolvedFunction(ctx.query_context, "equals", {key_expr->clone(), args[i]->clone()}));
            multi_if_args.push_back(args[i + 1]->clone());
        }
        multi_if_args.push_back(makeMapDefaultConstant(value_type));

        const String cond_fn = (multi_if_args.size() == 3) ? "if" : "multiIf";
        QueryTreeNodePtr result = createResolvedFunction(ctx.query_context, cond_fn, std::move(multi_if_args));
        return localFoldOnly(result, ctx.query_context);
    }

    /// Rule 3: `mapFilter((k, v) -> pred, inner_map)`
    if (fn_name == "mapFilter" && args.size() == 2)
    {
        const auto * lambda = args[0]->as<LambdaNode>();
        if (lambda && lambda->getArguments().getNames().size() == 2 && !hasDisallowedRewriteBodyNodes(lambda->getExpression()))
        {
            const auto * lambda_args_ptr = &lambda->getArguments();
            const String & k_name = lambda->getArguments().getNames()[0];
            const String & v_name = lambda->getArguments().getNames()[1];
            const auto & pred = lambda->getExpression();
            const auto & inner_map = args[1];

            ctx.applied_rule = true;
            const bool uses_v = referencesLambdaParam(pred, lambda_args_ptr, v_name);

            if (!uses_v)
            {
                QueryTreeNodePtr pred_k = substituteAndFoldExpr(
                    pred, lambda_args_ptr, k_name, key_expr, nullptr, {}, nullptr, ctx.query_context);

                if (auto b = tryGetConstBool(pred_k))
                {
                    if (!*b)
                        return (kind == MapAccessKind::ContainsKey) ? makeConstUInt8(false) : makeMapDefaultConstant(value_type);
                    return tryPushMapAccess(inner_map, key_expr, kind, ctx, depth + 1);
                }

                QueryTreeNodePtr inner_res = tryPushMapAccess(inner_map, key_expr, kind, ctx, depth + 1);
                if (!inner_res)
                    return nullptr;

                if (kind == MapAccessKind::ContainsKey)
                    return simplifyLocalNode(createResolvedFunction(ctx.query_context, "and", {pred_k, inner_res}), ctx.query_context);

                return simplifyLocalNode(
                    createResolvedFunction(ctx.query_context, "if", {pred_k, inner_res, makeMapDefaultConstant(value_type)}),
                    ctx.query_context);
            }

            QueryTreeNodePtr inner_has = tryPushMapAccess(inner_map, key_expr, MapAccessKind::ContainsKey, ctx, depth + 1);
            if (!inner_has)
                return nullptr;
            if (auto has_const = tryGetConstBool(inner_has); has_const && !*has_const)
                return (kind == MapAccessKind::ContainsKey) ? makeConstUInt8(false) : makeMapDefaultConstant(value_type);

            QueryTreeNodePtr inner_val = tryPushMapAccess(inner_map, key_expr, MapAccessKind::ArrayElement, ctx, depth + 1);
            if (!inner_val)
                return nullptr;

            QueryTreeNodePtr pred_kv = substituteAndFoldExpr(
                pred, lambda_args_ptr, k_name, key_expr, lambda_args_ptr, v_name, inner_val, ctx.query_context);
            QueryTreeNodePtr keep_cond = simplifyLocalNode(
                createResolvedFunction(ctx.query_context, "and", {inner_has, pred_kv}), ctx.query_context);

            if (auto b = tryGetConstBool(keep_cond))
            {
                if (!*b)
                    return (kind == MapAccessKind::ContainsKey) ? makeConstUInt8(false) : makeMapDefaultConstant(value_type);
                return (kind == MapAccessKind::ContainsKey) ? makeConstUInt8(true) : inner_val;
            }

            if (kind == MapAccessKind::ContainsKey)
                return keep_cond;

            return simplifyLocalNode(
                createResolvedFunction(ctx.query_context, "if", {keep_cond, inner_val, makeMapDefaultConstant(value_type)}),
                ctx.query_context);
        }
    }

    /// Rule 4: `mapApply((k, v) -> (out_k, out_v), inner_map)`
    if (fn_name == "mapApply" && args.size() == 2)
    {
        const auto * lambda = args[0]->as<LambdaNode>();
        const auto * tuple_fn = (lambda && lambda->getArguments().getNames().size() == 2)
            ? lambda->getExpression()->as<FunctionNode>()
            : nullptr;

        if (tuple_fn && tuple_fn->getFunctionName() == "tuple" && tuple_fn->getArguments().getNodes().size() == 2
            && !hasDisallowedRewriteBodyNodes(lambda->getExpression()))
        {
            const auto * lambda_args_ptr = &lambda->getArguments();
            const String & k_name = lambda->getArguments().getNames()[0];
            const String & v_name = lambda->getArguments().getNames()[1];
            const auto & out_k = tuple_fn->getArguments().getNodes()[0];
            const auto & out_v = tuple_fn->getArguments().getNodes()[1];
            const auto & inner_map = args[1];

            QueryTreeNodePtr effective_map = inner_map;
            QueryTreeNodePtr effective_key = key_expr;
            bool matched_shape = false;

            if (isLambdaParamNode(out_k, lambda_args_ptr, k_name))
            {
                matched_shape = true;
            }
            else
            {
                String prefix;
                if (QueryTreeNodePtr base_map = tryMatchPrefixExtract(out_k, lambda_args_ptr, k_name, inner_map, prefix))
                {
                    if (!referencesLambdaParam(out_v, lambda_args_ptr, k_name))
                    {
                        auto prefix_node = std::make_shared<ConstantNode>(Field(prefix), std::make_shared<DataTypeString>());
                        effective_key = localFoldOnly(
                            createResolvedFunction(ctx.query_context, "concat", {std::move(prefix_node), key_expr->clone()}),
                            ctx.query_context);
                        effective_map = std::move(base_map);
                        matched_shape = true;
                    }
                }
            }

            if (matched_shape)
            {
                ctx.applied_rule = true;
                if (kind == MapAccessKind::ContainsKey)
                    return tryPushMapAccess(effective_map, effective_key, MapAccessKind::ContainsKey, ctx, depth + 1);

                QueryTreeNodePtr out_v_k = substituteAndFoldExpr(
                    out_v, lambda_args_ptr, k_name, effective_key, nullptr, {}, nullptr, ctx.query_context);

                if (isLambdaParamNode(out_v_k, lambda_args_ptr, v_name))
                    return tryPushMapAccess(effective_map, effective_key, MapAccessKind::ArrayElement, ctx, depth + 1);

                if (isDefaultConstantForType(out_v_k, value_type))
                    return out_v_k;

                QueryTreeNodePtr inner_has = tryPushMapAccess(effective_map, effective_key, MapAccessKind::ContainsKey, ctx, depth + 1);
                if (!inner_has)
                    return nullptr;
                if (auto has_const = tryGetConstBool(inner_has); has_const && !*has_const)
                    return makeMapDefaultConstant(value_type);

                QueryTreeNodePtr out_v_final = out_v_k;
                if (referencesLambdaParam(out_v_k, lambda_args_ptr, v_name))
                {
                    QueryTreeNodePtr inner_val = tryPushMapAccess(
                        effective_map, effective_key, MapAccessKind::ArrayElement, ctx, depth + 1);
                    if (!inner_val)
                        return nullptr;
                    out_v_final = substituteAndFoldExpr(
                        out_v_k, nullptr, {}, nullptr, lambda_args_ptr, v_name, inner_val, ctx.query_context);
                }

                return simplifyLocalNode(
                    createResolvedFunction(ctx.query_context, "if", {inner_has, out_v_final, makeMapDefaultConstant(value_type)}),
                    ctx.query_context);
            }
        }
    }

    /// Rule 5: `mapUpdate(left_map, right_map)` -> right overrides left on matching key
    if (fn_name == "mapUpdate" && args.size() == 2)
    {
        ctx.applied_rule = true;
        const auto & left_map = args[0];
        const auto & right_map = args[1];

        QueryTreeNodePtr right_has = tryPushMapAccess(right_map, key_expr, MapAccessKind::ContainsKey, ctx, depth + 1);
        if (!right_has)
            return nullptr;
        auto right_has_const = tryGetConstBool(right_has);

        if (kind == MapAccessKind::ContainsKey)
        {
            if (right_has_const && *right_has_const)
                return makeConstUInt8(true);

            QueryTreeNodePtr left_has = tryPushMapAccess(left_map, key_expr, MapAccessKind::ContainsKey, ctx, depth + 1);
            if (!left_has)
                return nullptr;
            if (right_has_const && !*right_has_const)
                return left_has;
            if (auto left_has_const = tryGetConstBool(left_has); left_has_const && !*left_has_const)
                return right_has;
            return simplifyLocalNode(createResolvedFunction(ctx.query_context, "or", {right_has, left_has}), ctx.query_context);
        }

        if (right_has_const && *right_has_const)
            return tryPushMapAccess(right_map, key_expr, MapAccessKind::ArrayElement, ctx, depth + 1);
        if (right_has_const && !*right_has_const)
            return tryPushMapAccess(left_map, key_expr, MapAccessKind::ArrayElement, ctx, depth + 1);

        QueryTreeNodePtr left_val = tryPushMapAccess(left_map, key_expr, MapAccessKind::ArrayElement, ctx, depth + 1);
        QueryTreeNodePtr right_val = tryPushMapAccess(right_map, key_expr, MapAccessKind::ArrayElement, ctx, depth + 1);
        if (!left_val || !right_val)
            return nullptr;

        if (isDefaultConstantForType(left_val, value_type))
            return right_val;

        return simplifyLocalNode(createResolvedFunction(ctx.query_context, "if", {right_has, right_val, left_val}), ctx.query_context);
    }

    /// Rule 6: `if` / `multiIf` returning Map
    if (fn_name == "if" && args.size() == 3)
    {
        ctx.applied_rule = true;
        if (auto cond_const = tryGetConstBool(args[0]))
            return tryPushMapAccess(*cond_const ? args[1] : args[2], key_expr, kind, ctx, depth + 1);

        QueryTreeNodePtr true_res = tryPushMapAccess(args[1], key_expr, kind, ctx, depth + 1);
        QueryTreeNodePtr false_res = tryPushMapAccess(args[2], key_expr, kind, ctx, depth + 1);
        if (!true_res || !false_res)
            return nullptr;
        return simplifyLocalNode(
            createResolvedFunction(ctx.query_context, "if", {args[0]->clone(), true_res, false_res}),
            ctx.query_context);
    }

    if (fn_name == "multiIf" && args.size() >= 3 && (args.size() % 2 == 1))
    {
        ctx.applied_rule = true;
        QueryTreeNodes new_args;
        new_args.reserve(args.size());
        for (size_t i = 0; i + 1 < args.size(); i += 2)
        {
            auto b = tryGetConstBool(args[i]);
            if (b && !*b)
                continue;
            if (b && *b)
            {
                if (new_args.empty())
                    return tryPushMapAccess(args[i + 1], key_expr, kind, ctx, depth + 1);
                QueryTreeNodePtr branch_res = tryPushMapAccess(args[i + 1], key_expr, kind, ctx, depth + 1);
                if (!branch_res)
                    return nullptr;
                new_args.push_back(std::move(branch_res));
                return simplifyLocalNode(
                    createResolvedFunction(ctx.query_context, (new_args.size() == 3) ? "if" : "multiIf", std::move(new_args)),
                    ctx.query_context);
            }
            QueryTreeNodePtr branch_res = tryPushMapAccess(args[i + 1], key_expr, kind, ctx, depth + 1);
            if (!branch_res)
                return nullptr;
            new_args.push_back(args[i]->clone());
            new_args.push_back(std::move(branch_res));
        }

        QueryTreeNodePtr else_res = tryPushMapAccess(args.back(), key_expr, kind, ctx, depth + 1);
        if (!else_res)
            return nullptr;
        if (new_args.empty())
            return else_res;
        new_args.push_back(std::move(else_res));
        return simplifyLocalNode(
            createResolvedFunction(ctx.query_context, (new_args.size() == 3) ? "if" : "multiIf", std::move(new_args)),
            ctx.query_context);
    }

    return buildBaseMapAccess(map_expr, key_expr, kind, ctx.query_context);
}

bool containsNonTargetTableColumn(
    const QueryTreeNodePtr & node,
    const TableNode * target_table,
    UnorderedSetWithMemoryTracking<const IQueryTreeNode *> & inner_lambda_args)
{
    if (!node || isQueryOrUnionNode(node))
        return false;

    if (const auto * lambda = node->as<LambdaNode>())
    {
        const auto * lambda_args_ptr = &lambda->getArguments();
        inner_lambda_args.insert(lambda_args_ptr);
        bool res = containsNonTargetTableColumn(lambda->getExpression(), target_table, inner_lambda_args);
        inner_lambda_args.erase(lambda_args_ptr);
        return res;
    }

    if (const auto * col_node = node->as<ColumnNode>())
    {
        auto src = col_node->getColumnSourceOrNull();
        if (src)
        {
            if (src.get() != target_table && !inner_lambda_args.contains(src.get()))
                return true;
        }
    }

    for (const auto & child : node->getChildren())
    {
        if (child && containsNonTargetTableColumn(child, target_table, inner_lambda_args))
            return true;
    }
    return false;
}

void propagateAccessCheckColumn(
    const QueryTreeNodePtr & node,
    const TableNode * target_table,
    const String & access_check_col)
{
    if (!node || isQueryOrUnionNode(node))
        return;

    if (auto * col_node = node->as<ColumnNode>())
    {
        auto src = col_node->getColumnSourceOrNull();
        if (src && src.get() == target_table && !col_node->getAccessCheckColumnName())
            col_node->setAccessCheckColumnName(access_check_col);
    }

    for (const auto & child : node->getChildren())
    {
        if (child)
            propagateAccessCheckColumn(child, target_table, access_check_col);
    }
}

class AliasMapPushdownVisitor : public InDepthQueryTreeVisitorWithContext<AliasMapPushdownVisitor>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<AliasMapPushdownVisitor>;
    using Base::Base;

    static bool needChildVisit(QueryTreeNodePtr & parent, QueryTreeNodePtr &)
    {
        /// Keep table-function arguments untouched, same as other optimizer passes.
        return parent->getNodeType() != QueryTreeNodeType::TABLE_FUNCTION;
    }

    void leaveImpl(QueryTreeNodePtr & node)
    {
        if (!getSettings()[Setting::optimize_map_alias_pushdown])
            return;

        auto * fn = node->as<FunctionNode>();
        if (!fn || !fn->isResolved() || !fn->isOrdinaryCall())
            return;

        std::optional<MapAccessKind> kind;
        const String & name = fn->getFunctionName();
        if (name == "arrayElement")
            kind = MapAccessKind::ArrayElement;
        else if (name == "has" || name == "mapContainsKey")
            kind = MapAccessKind::ContainsKey;
        else
            return;

        const auto & args = fn->getArguments().getNodes();
        if (args.size() != 2)
            return;

        const auto * alias_col = args[0]->as<ColumnNode>();
        if (!alias_col || !alias_col->hasExpression() || !isMap(alias_col->getColumnType()))
            return;

        auto column_source = alias_col->getColumnSourceOrNull();
        const auto * table_node = column_source ? column_source->as<TableNode>() : nullptr;
        if (!table_node || table_node->getStorage()->isRemote())
            return;

        if (alias_col->getResultType()->isNullable() || fn->isConvertedToNullable())
            return;

        PushdownContext ctx{getContext(), table_node, false};
        QueryTreeNodePtr pushed_expr = tryPushMapAccess(alias_col->getExpression(), args[1], *kind, ctx, 0);
        if (!pushed_expr || !ctx.applied_rule)
            return;

        DataTypePtr original_result_type = fn->getResultType();
        if (!pushed_expr->getResultType()->equals(*original_result_type))
            pushed_expr = tryFoldResolvedFunction(buildCastFunction(pushed_expr, original_result_type, getContext(), true));

        const String access_check_col = alias_col->getAccessCheckColumnName().value_or(alias_col->getColumnName());
        UnorderedSetWithMemoryTracking<const IQueryTreeNode *> inner_lambda_args;
        if (containsNonTargetTableColumn(pushed_expr, table_node, inner_lambda_args))
        {
            /// The pushed expression references columns from outside `table_node` (e.g. ARRAY JOIN or another JOIN table),
            /// so it cannot live inside a `table_node` ALIAS column wrapper that is computed before joins/ARRAY JOIN.
            /// Emit the pushed expression directly and tag `table_node` source columns for ALIAS RBAC delegation.
            propagateAccessCheckColumn(pushed_expr, table_node, access_check_col);
            node = std::move(pushed_expr);
            return;
        }

        const auto expr_hash = pushed_expr->getTreeHash();
        const String synthetic_name = fmt::format("__alias_pushdown_{}_{:016x}_{:016x}", access_check_col, expr_hash.low64, expr_hash.high64);

        auto wrapped = std::make_shared<ColumnNode>(
            NameAndTypePair{synthetic_name, original_result_type},
            std::move(pushed_expr),
            column_source);
        wrapped->setAccessCheckColumnName(access_check_col);
        node = std::move(wrapped);
    }
};

}

void AliasMapPushdownPass::run(QueryTreeNodePtr & query_tree_node, ContextPtr context)
{
    if (!context->getSettingsRef()[Setting::optimize_map_alias_pushdown])
        return;

    AliasMapPushdownVisitor visitor(std::move(context));
    visitor.visit(query_tree_node);
}

}
