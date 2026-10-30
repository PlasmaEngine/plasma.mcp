#pragma once

#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

#include <Foundation/Containers/Map.h>
#include <Foundation/Types/Variant.h>

#include <EditorFramework/IPC/EngineProcessConnection.h>

/// \brief Live insight into the engine process: the full stats map, per-allocator memory, cvars,
/// console commands and profiling captures.
///
/// Everything here needs the engine process (the one that renders viewports and runs simulation) to be
/// running. The stats, memory, capture and console tools are blocking request/response round trips
/// through the engine's message pump; the cvar tools work from a cache that the engine's own cvar
/// broadcast fills, so listing is free and only setting talks to the engine.
class plMcpProfilingTool : public plMcpToolProvider
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpProfilingTool, plMcpToolProvider);

public:
  plMcpProfilingTool();
  ~plMcpProfilingTool();

  virtual void OnActivate() override;
  virtual void OnDeactivate() override;

  virtual void GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const override;
  virtual void Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result) override;

private:
  struct CVarInfo
  {
    plString m_sPlugin;
    plString m_sDescription;
    plVariant m_Value;
  };

  void ConnectionEventHandler(const plEditorEngineProcessConnection::Event& e);

  /// Returns the connection if the engine process is up, otherwise writes an error naming the fix.
  static plEditorEngineProcessConnection* GetEngineConnection(plMcpToolResult& out_result);

  /// Asks the engine to re-broadcast every cvar and pumps until the broadcast is in m_CVars. Needed
  /// because this plugin loads with the project, which can be after the engine's startup broadcast.
  void RefreshCVarCache(plEditorEngineProcessConnection* pConnection);

  void ExecuteStats(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteMemory(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteProfileCapture(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteCVarList(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteCVarSet(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteConsole(const plVariantDictionary& arguments, plMcpToolResult& out_result);

  /// Filled by the engine's cvar broadcast (on engine startup, plugin load and every change), which all
  /// arrives on the main thread. Cleared when the engine process goes away, because the values would be
  /// stale and a restarted engine re-broadcasts everything anyway.
  plMap<plString, CVarInfo> m_CVars;

  /// Whether a full re-broadcast was pulled from the current engine process. Individual value-change
  /// broadcasts fill m_CVars without making it complete, so emptiness cannot stand in for this.
  bool m_bCVarCacheComplete = false;

  bool m_bSubscribed = false;
};
