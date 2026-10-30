#pragma once

#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

#include <Foundation/Containers/Deque.h>
#include <Foundation/Logging/Log.h>
#include <Foundation/Threading/Mutex.h>

/// \brief Writing to and reading back the editor log.
///
/// Reading matters more than writing: it is the cheapest feedback loop an agent has. It can trigger
/// something in the editor and then look at what the editor said about it, instead of asking the user
/// to copy the log.
class plMcpLogTool : public plMcpToolProvider
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpLogTool, plMcpToolProvider);

public:
  plMcpLogTool();
  ~plMcpLogTool();

  virtual void OnActivate() override;
  virtual void OnDeactivate() override;

  virtual void GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const override;
  virtual void Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result) override;

private:
  struct Entry
  {
    plUInt64 m_uiId = 0;
    plLogMsgType::Enum m_Type = plLogMsgType::None;
    plString m_sText;
  };

  void LogEventHandler(const plLoggingEventData& e);

  void ExecuteWrite(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteRead(const plVariantDictionary& arguments, plMcpToolResult& out_result);

  /// How many messages are kept. The editor logs a lot, so this is a compromise between covering a
  /// whole operation and not holding on to megabytes of text.
  static constexpr plUInt32 s_uiMaxEntries = 2000;

  /// Log events arrive from whatever thread produced them, everything else runs on the main thread.
  mutable plMutex m_Mutex;
  plDeque<Entry> m_Entries;
  plEventSubscriptionID m_LogSubscription = 0;

  /// Ids are assigned in order and never reused, even once their entry falls out of m_Entries, so a
  /// 'sinceId' from an earlier read remains meaningful no matter how much has been logged since.
  plUInt64 m_uiNextId = 1;
};
