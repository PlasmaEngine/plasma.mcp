#pragma once

#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

class plDocument;
class plMcpJsonWriter;
struct plDocumentTypeDescriptor;

/// \brief Lists, opens, creates, saves, closes and focuses editor documents.
///
/// 'document_types' is the type-level view - what can be created and with which file extension - and
/// is the call that has to come first, because every other tool here names a document type or a path
/// whose extension decides the type.
///
/// The tools that change something are deliberately explicit about it: closing discards unsaved work
/// unless told otherwise, and deleting rewrites other documents. Neither can ask, so both take the
/// decision as an argument (see Status.md, 'A tool must never wait for a human').
class plMcpDocumentTool : public plMcpToolProvider
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpDocumentTool, plMcpToolProvider);

public:
  virtual void GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const override;
  virtual void Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result) override;

private:
  void ExecuteListTypes(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteList(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteOpen(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteCreate(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteSave(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteClose(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteFocus(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteDelete(const plVariantDictionary& arguments, plMcpToolResult& out_result);

  /// Writes guid, path, type and modified state of one open document. Every tool here returns this
  /// for the document it acted on, so a caller never has to follow up with 'document_list' to learn
  /// the guid of something it just created.
  static void WriteDocumentIdentity(plMcpJsonWriter& ref_writer, const plDocument& document);
};
