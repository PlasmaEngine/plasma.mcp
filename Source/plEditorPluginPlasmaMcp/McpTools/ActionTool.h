#pragma once

#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

struct plActionDescriptor;

/// \brief Tools for listing and triggering editor actions - the things behind menu entries and toolbar buttons.
///
/// Restricted to actions of plActionScope::Global. Document and window actions need a context that a tool
/// call does not have, and their state depends on the selection, so they are listed but neither their
/// state nor their execution is offered.
class plMcpActionTool : public plMcpToolProvider
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpActionTool, plMcpToolProvider);

public:
  virtual void GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const override;
  virtual void Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result) override;

private:
  void ExecuteList(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteState(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteAction(const plVariantDictionary& arguments, plMcpToolResult& out_result);

  /// \brief Resolves an action by name, optionally restricted to a category.
  ///
  /// Without a category the name has to be unique across all of them, which is not guaranteed: the same
  /// name may be registered in several categories. out_bAmbiguous reports that case separately from
  /// 'not found', so the caller can ask for a category instead of just failing.
  static const plActionDescriptor* FindAction(plStringView sName, plStringView sCategory, bool& out_bAmbiguous);
};
