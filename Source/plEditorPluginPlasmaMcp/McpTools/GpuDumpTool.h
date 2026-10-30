#pragma once
#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>
class plMcpConnection;
class plMcpGpuDumpTool : public plMcpToolProvider
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpGpuDumpTool, plMcpToolProvider);

public:
  ~plMcpGpuDumpTool();

  void GetSupportedTools(plDynamicArray<plMcpToolDesc>& tools) const override;
  void Execute(plStringView tool, const plVariantDictionary& arguments, plMcpToolResult& result) override;

  /// Answers a GET of the dump viewer's files (/gpudump/<dump>/<file>); false for other paths.
  static bool Serve(plMcpConnection& ref_connection, plStringView sPath);
};
