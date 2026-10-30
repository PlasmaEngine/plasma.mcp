#pragma once

#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

/// \brief Reading and editing node graph documents: animation graphs, visual scripts, visual shaders,
/// state machines - anything whose object manager is an plVisualGraphObjectManager.
///
/// The generic object tools cannot do this. A connection is not a property an agent can set: it is a
/// document object whose identity the node manager tracks separately, created through
/// plConnectNodePinsCommand so that pin validation, cycle detection and undo all happen. Adding a raw
/// connection object instead produces something the manager does not recognise as a connection at all.
/// Node positions are the same story - they live in node meta data, not in reflected properties.
///
/// So graphs get their own tools. Nodes themselves are still created with 'object_modify' /
/// 'addObject' into the root's Children, and configured with ordinary property sets.
class plMcpNodeGraphTool : public plMcpToolProvider
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpNodeGraphTool, plMcpToolProvider);

public:
  plMcpNodeGraphTool();
  ~plMcpNodeGraphTool();

  virtual void GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const override;
  virtual void Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result) override;

private:
  void ExecutePins(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteConnect(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteDisconnect(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteMove(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteRead(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteLayout(const plVariantDictionary& arguments, plMcpToolResult& out_result);
};
