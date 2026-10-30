#pragma once

#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

/// \brief Information about the project that the editor currently has open.
///
/// This is what orients an agent that has just connected: every path an other tool returns is
/// relative to the data directories reported here, and asset states depend on the active profile.
///
/// 'package_list' is the same question asked of the plugin packages: project_info reports which
/// plugins are loaded, this reports where each of them came from. The two are separate because
/// project_info is called routinely and scanning the package store is file system work.
class plMcpProjectTool : public plMcpToolProvider
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpProjectTool, plMcpToolProvider);

public:
  virtual void GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const override;
  virtual void Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result) override;

private:
  void ExecuteProjectInfo(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecutePackageList(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteProjectExport(const plVariantDictionary& arguments, plMcpToolResult& out_result);
};
