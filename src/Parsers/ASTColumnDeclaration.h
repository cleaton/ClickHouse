#pragma once

#include <Parsers/IAST.h>
#include <Storages/ColumnDefault.h>

namespace DB
{

/// Default specifier for column declarations - compatible with ColumnDefaultKind plus Empty and AutoIncrement
enum class ColumnDefaultSpecifier : UInt8
{
    Empty = 0,
    Default,
    Materialized,
    Alias,
    Ephemeral,
    Proxy,
    AutoIncrement
};

const char * toString(ColumnDefaultSpecifier kind);
ColumnDefaultSpecifier columnDefaultSpecifierFromString(std::string_view str);
ColumnDefaultSpecifier toColumnDefaultSpecifier(ColumnDefaultKind kind);
ColumnDefaultKind toColumnDefaultKind(ColumnDefaultSpecifier specifier);

/** Name, type, default-specifier, default-expression, comment-expression.
 *  The type is optional if default-expression is specified.
 */
class ASTColumnDeclaration : public IAST
{
public:
    String name;
    ColumnDefaultSpecifier default_specifier = ColumnDefaultSpecifier::Empty;

    std::optional<bool> null_modifier;
    bool ephemeral_default : 1 = false;
    bool primary_key_specifier : 1 = false;

private:
    /// Pack child indices (4 bits each) into a single integer. 0xF means "not set".
    static constexpr UInt8 kNotSet = 0xF;
    static constexpr UInt64 kAllNotSet = 0xFFFFFFFFF;

    /// Bit positions for each index (4 bits each)
    enum IndexSlot : UInt8
    {
        TYPE = 0,
        DEFAULT_EXPR = 4,
        COMMENT = 8,
        CODEC = 12,
        STATS = 16,
        TTL = 20,
        COLLATION = 24,
        SETTINGS = 28,
        PROXY_ELEMENT = 32
    };

    UInt64 packed_indices = kAllNotSet;

    UInt8 getIndex(IndexSlot slot) const { return (packed_indices >> slot) & 0xF; }
    void setIndex(IndexSlot slot, UInt8 val) { packed_indices = (packed_indices & ~(0xFULL << slot)) | (static_cast<UInt64>(val) << slot); }

    ASTPtr getChildOrNull(IndexSlot slot) const
    {
        UInt8 idx = getIndex(slot);
        return idx == kNotSet ? nullptr : children[idx];
    }

    void setChild(IndexSlot slot, ASTPtr node)
    {
        if (!node)
        {
            UInt8 idx = getIndex(slot);
            if (idx != kNotSet)
            {
                children.erase(children.begin() + idx);
                setIndex(slot, kNotSet);
                // After erase, we must update all other indices that were pointing beyond this one
                for (UInt8 s : {TYPE, DEFAULT_EXPR, COMMENT, CODEC, STATS, TTL, COLLATION, SETTINGS, PROXY_ELEMENT})
                {
                    UInt8 other_idx = getIndex(static_cast<IndexSlot>(s));
                    if (other_idx != kNotSet && other_idx > idx)
                        setIndex(static_cast<IndexSlot>(s), other_idx - 1);
                }
            }
            return;
        }

        UInt8 idx = getIndex(slot);
        if (idx != kNotSet)
            children[idx] = std::move(node);
        else
        {
            /// Ensure we don't exceed 4 bits for index
            if (children.size() >= kNotSet)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Too many children in ASTColumnDeclaration");

            setIndex(slot, static_cast<UInt8>(children.size()));
            children.push_back(std::move(node));
        }
    }

public:
    bool hasChildren() const { return !children.empty(); }
    ASTPtr getType() const { return getChildOrNull(TYPE); }
    ASTPtr getDefaultExpression() const { return getChildOrNull(DEFAULT_EXPR); }
    ASTPtr getComment() const { return getChildOrNull(COMMENT); }
    ASTPtr getCodec() const { return getChildOrNull(CODEC); }
    ASTPtr getStatisticsDesc() const { return getChildOrNull(STATS); }
    ASTPtr getTTL() const { return getChildOrNull(TTL); }
    ASTPtr getCollation() const { return getChildOrNull(COLLATION); }
    ASTPtr getSettings() const { return getChildOrNull(SETTINGS); }
    ASTPtr getProxyElementExpression() const { return getChildOrNull(PROXY_ELEMENT); }

    void setType(ASTPtr node) { setChild(TYPE, std::move(node)); }
    void setDefaultExpression(ASTPtr node) { setChild(DEFAULT_EXPR, std::move(node)); }
    void setComment(ASTPtr node) { setChild(COMMENT, std::move(node)); }
    void setCodec(ASTPtr node) { setChild(CODEC, std::move(node)); }
    void setStatisticsDesc(ASTPtr node) { setChild(STATS, std::move(node)); }
    void setTTL(ASTPtr node) { setChild(TTL, std::move(node)); }
    void setCollation(ASTPtr node) { setChild(COLLATION, std::move(node)); }
    void setSettings(ASTPtr node) { setChild(SETTINGS, std::move(node)); }
    void setProxyElementExpression(ASTPtr node) { setChild(PROXY_ELEMENT, std::move(node)); }

    String getID(char delim) const override { return "ColumnDeclaration" + (delim + name); }

    ASTPtr clone() const override;


protected:
    void formatImpl(WriteBuffer & ostr, const FormatSettings & format_settings, FormatState & state, FormatStateStacked frame) const override;
    void forEachPointerToChild(std::function<void(IAST **, boost::intrusive_ptr<IAST> *)> f) override;

private:
    using IAST::children; /// Don't let other manipulate children directly
};

}
