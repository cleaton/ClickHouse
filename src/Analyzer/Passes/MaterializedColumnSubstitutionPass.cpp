#include <Analyzer/Passes/MaterializedColumnSubstitutionPass.h>

#include <Access/Common/AccessType.h>
#include <Access/ContextAccess.h>
#include <Analyzer/ColumnNode.h>
#include <Analyzer/ConstantNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/InDepthQueryTreeVisitor.h>
#include <Analyzer/TableNode.h>
#include <Common/UnorderedMapWithMemoryTracking.h>
#include <Core/Field.h>
#include <Core/NamesAndTypes.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/IDataType.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/IStorage.h>
#include <Storages/StorageSnapshot.h>

#include <functional>

namespace DB
{

namespace Setting
{
    extern const SettingsBool optimize_use_materialized_columns;
}

namespace
{

struct MapKeyPair
{
    String column_name;
    Field key;

    bool operator==(const MapKeyPair & other) const = default;
};

struct MapKeyPairHash
{
    size_t operator()(const MapKeyPair & pair) const
    {
        SipHash hash;
        hash.update(pair.column_name.size());
        hash.update(pair.column_name);
        hash.update(pair.key.dump());
        return hash.get64();
    }
};

using MaterializedLookupMap = UnorderedMapWithMemoryTracking<MapKeyPair, NameAndTypePair, MapKeyPairHash>;

MaterializedLookupMap buildMaterializedMapForTable(const TableNode & table_node)
{
    MaterializedLookupMap result;
    const auto & storage = table_node.getStorage();
    if (!storage || !storage->isMergeTree() || storage->isRemote() || storage->getName() == "Alias")
        return result;

    const auto & metadata = table_node.getStorageSnapshot()->metadata;
    if (!metadata)
        return result;

    const auto & columns = metadata->getColumns();
    for (const auto & col : columns)
    {
        const auto & default_desc = columns.getDefault(col.name);
        if (!default_desc || default_desc->kind != ColumnDefaultKind::Materialized || !default_desc->expression)
            continue;

        const auto * fn = default_desc->expression->as<ASTFunction>();
        if (!fn || fn->name != "arrayElement" || !fn->arguments || fn->arguments->children.size() != 2)
            continue;

        const auto * base_id = fn->arguments->children[0]->as<ASTIdentifier>();
        const auto * key_lit = fn->arguments->children[1]->as<ASTLiteral>();
        if (!base_id || !base_id->isShort() || !key_lit || key_lit->value.isNull())
            continue;

        const String & base_col_name = base_id->shortName();
        if (!columns.hasPhysical(base_col_name))
            continue;

        const auto base_col_type = columns.getPhysical(base_col_name).type;
        if (!isMap(base_col_type))
            continue;

        const auto value_type = assert_cast<const DataTypeMap &>(*base_col_type).getValueType();
        if (!col.type->equals(*value_type))
            continue;

        result.try_emplace(MapKeyPair{base_col_name, key_lit->value}, NameAndTypePair{col.name, col.type});
    }

    return result;
}

class MaterializedColumnSubstitutionVisitor : public InDepthQueryTreeVisitorWithContext<MaterializedColumnSubstitutionVisitor>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<MaterializedColumnSubstitutionVisitor>;
    using Base::Base;

    static bool needChildVisit(QueryTreeNodePtr & parent, QueryTreeNodePtr &)
    {
        return parent->getNodeType() != QueryTreeNodeType::TABLE_FUNCTION;
    }

    void enterImpl(QueryTreeNodePtr & node)
    {
        if (const auto * col_node = node->as<ColumnNode>(); col_node && col_node->hasExpression())
            ++alias_depth;
    }

    void leaveImpl(QueryTreeNodePtr & node)
    {
        if (const auto * col_node = node->as<ColumnNode>(); col_node && col_node->hasExpression())
        {
            --alias_depth;
            return;
        }

        if (!getSettings()[Setting::optimize_use_materialized_columns])
            return;

        const auto * fn = node->as<FunctionNode>();
        if (!fn || !fn->isResolved() || !fn->isOrdinaryCall() || fn->getFunctionName() != "arrayElement")
            return;

        const auto & args = fn->getArguments().getNodes();
        if (args.size() != 2)
            return;

        const auto * map_col = args[0]->as<ColumnNode>();
        const auto * key_const = args[1]->as<ConstantNode>();
        if (!map_col || map_col->hasExpression() || !key_const || key_const->isNull())
            return;

        auto column_source = map_col->getColumnSourceOrNull();
        const auto * table_node = column_source ? column_source->as<TableNode>() : nullptr;
        if (!table_node)
            return;

        auto it = table_maps.find(table_node);
        if (it == table_maps.end())
            it = table_maps.emplace(table_node, buildMaterializedMapForTable(*table_node)).first;

        const auto & lookup = it->second;
        if (lookup.empty())
            return;

        auto mat_it = lookup.find(MapKeyPair{map_col->getColumnName(), key_const->getValue()});
        if (mat_it == lookup.end())
            return;

        const NameAndTypePair & mat_col = mat_it->second;
        if (!mat_col.type->equals(*fn->getResultType()))
            return;

        /// Outside an ALIAS column expression (and when RBAC is not delegated to an ALIAS column),
        /// only substitute if the user holds SELECT access on the target MATERIALIZED column.
        const auto & delegated_access_col = map_col->getAccessCheckColumnName();
        if (alias_depth == 0 && !delegated_access_col)
        {
            const auto & storage_id = table_node->getStorageID();
            if (!getContext()->getAccess()->isGranted(
                    AccessType::SELECT, storage_id.getDatabaseName(), storage_id.getTableName(), mat_col.name))
                return;
        }

        auto mat_node = std::make_shared<ColumnNode>(mat_col, column_source);
        if (delegated_access_col)
            mat_node->setAccessCheckColumnName(*delegated_access_col);
        node = std::move(mat_node);
    }

private:
    size_t alias_depth = 0;
    UnorderedMapWithMemoryTracking<const TableNode *, MaterializedLookupMap> table_maps;
};

}

void MaterializedColumnSubstitutionPass::run(QueryTreeNodePtr & query_tree_node, ContextPtr context)
{
    if (!context->getSettingsRef()[Setting::optimize_use_materialized_columns])
        return;

    MaterializedColumnSubstitutionVisitor visitor(std::move(context));
    visitor.visit(query_tree_node);
}

}
