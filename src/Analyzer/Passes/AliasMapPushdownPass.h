#pragma once

#include <Analyzer/IQueryTreePass.h>

namespace DB
{

/** Pushes `Map` lookups (`arrayElement`, `has`, `mapContainsKey`) into `ALIAS` column
  * expressions (`mapFilter`, `mapApply`, `mapUpdate`, `if`, `multiIf`, literal `map`)
  * before subcolumn and materialized-column optimizations, while preserving column-level
  * `SELECT` access checks on the `ALIAS` column.
  *
  * Enabled with setting `optimize_map_alias_pushdown`.
  */
class AliasMapPushdownPass final : public IQueryTreePass
{
public:
    String getName() override { return "AliasMapPushdown"; }

    String getDescription() override
    {
        return "Push Map lookups (arrayElement, has, mapContainsKey) into ALIAS column expressions";
    }

    void run(QueryTreeNodePtr & query_tree_node, ContextPtr context) override;
};

}
