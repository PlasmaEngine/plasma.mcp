#pragma once

#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

class plDocument;
class plDocumentObject;
class plMcpJsonWriter;
class plAbstractProperty;

/// \brief Reads and modifies the properties of the objects inside a document.
///
/// The unit of addressing is one object plus one property name. An object is named by its guid, or
/// implicitly: every tool here defaults to the document's top level object, which for the asset types
/// deriving from plSimpleAssetDocument is the single object holding all of the asset's settings.
///
/// Only direct properties are read. A property whose value is another object reports that object's
/// guid instead of its contents, so walking a tree is a sequence of calls rather than one large
/// response - the same listing/detail split the rest of the tools use.
///
/// 'object_modify' addresses an object through its parent and a property, which is the right shape
/// for editing a value but the wrong one for the tree itself: an agent holds the guid of an object,
/// not its index in its parent. 'object_delete' and 'object_reparent' take that guid directly.
///
/// 'object_select' is the editor's own notion of what the user is looking at. Without it an agent and
/// the user have no way to refer to the same object.
///
/// Modifications go through the document's plObjectCommandAccessor, so each one is a single undoable
/// transaction. Nothing here saves; that stays an explicit plMcpDocumentTool call.
class plMcpObjectTool : public plMcpToolProvider
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpObjectTool, plMcpToolProvider);

public:
  virtual void GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const override;
  virtual void Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result) override;

private:
  void ExecuteReadProperties(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteTree(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteModify(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteDelete(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteReparent(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteSelect(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteUndo(const plVariantDictionary& arguments, plMcpToolResult& out_result);

  /// Writes one property's name, category, type and current value.
  ///
  /// Containers report their element count and, for maps, their keys, but not their elements - those
  /// are a follow up call with an index. Values that are objects report a guid for the same reason.
  static void WritePropertyValue(plMcpJsonWriter& ref_writer, plObjectAccessorBase* pAccessor, const plDocumentObject* pObject,
    const plAbstractProperty* pProp, bool bIncludeValues);
};
