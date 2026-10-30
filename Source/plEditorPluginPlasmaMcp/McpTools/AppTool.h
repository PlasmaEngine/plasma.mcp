#pragma once

#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

class plMcpJsonWriter;

/// \brief Tools that act on the editor process itself, rather than on the project it has open.
///
/// These exist to close the automation loop: an agent that can launch an editor (with '-mcpport' to get
/// its own port), query it and then shut it down again can test a change without a human in between.
///
/// The 'engine_' tools are about the *other* process: the editor renders scenes and runs game
/// simulation in a separate engine process, which can crash, hang or be restarted on its own. From the
/// editor that failure looks like viewports going black, and nothing else here reports it.
class plMcpAppTool : public plMcpToolProvider
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpAppTool, plMcpToolProvider);

public:
  virtual void GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const override;
  virtual void Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result) override;

private:
  void ExecutePing(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteQuit(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteInfo(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteCommandLineOptions(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteEngineStatus(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteEngineRestart(const plVariantDictionary& arguments, plMcpToolResult& out_result);

  /// Writes the state of the engine process. Shared by engine_status and engine_restart, so that a
  /// restart reports the state it produced rather than only that it was asked for.
  static void WriteEngineStatus(plMcpJsonWriter& ref_writer);

  /// \brief Appends the names of all open documents with unsaved changes.
  static void CollectModifiedDocuments(plDynamicArray<plString>& out_documents);
};
