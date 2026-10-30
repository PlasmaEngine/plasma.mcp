#pragma once

#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

#include <EditorFramework/Assets/Declarations.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/Uuid.h>

struct plSubAsset;
struct plAssetProcessorProgressEvent;
class plMcpJsonWriter;

/// \brief Queries the asset database: which asset types exist, which assets exist, and how they relate.
///
/// The tools split along the cost of answering. 'asset_types' and 'asset_health' are type-level and
/// aggregate views that stay cheap on any project. 'asset_find' is the listing half - it narrows
/// thousands of assets down to a handful of GUIDs - and 'asset_info', 'asset_uses' and
/// 'asset_thumbnail' are the detail halves that an agent calls on those few.
///
/// 'asset_root_cause' is the exception to that split: a broken asset usually names another broken
/// asset, so answering "why is this broken" one asset_info call at a time takes as many calls as the
/// chain is long. It walks the chain in one go and returns only its ends.
///
/// 'asset_transform' is the only one here that changes anything, and the only one slow enough that a
/// caller has to think about the timeout (see Status.md, 'Tool calls block the editor').
class plMcpAssetTool : public plMcpToolProvider
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpAssetTool, plMcpToolProvider);

public:
  virtual void OnActivate() override;
  virtual void OnDeactivate() override;

  virtual void GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const override;
  virtual void Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result) override;

private:
  /// One finished asset transform, as recorded by asset_history.
  struct HistoryEntry
  {
    plUInt64 m_uiId = 0;
    plUuid m_Guid;
    plString m_sPath;
    plString m_sMessage;
    plTime m_Duration;
    plTime m_EndTime;
    plEnum<plTransformResult> m_Result;
    bool m_bBackground = true; ///< false for a transform this plugin triggered through asset_transform
  };

  void ProcessorProgressEventHandler(const plAssetProcessorProgressEvent& e);
  void RecordHistory(const plUuid& guid, plStringView sPath, const plTransformStatus& status, plTime duration, bool bBackground);

  void ExecuteListTypes(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteFind(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteInfo(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteRootCause(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteUses(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteThumbnail(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteTransform(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteTransformAll(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteHealth(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteProcessor(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteHistory(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteListImporters(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteImport(const plVariantDictionary& arguments, plMcpToolResult& out_result);

  /// Writes the identifying fields of one asset: guid, path, type and name. This is what a listing
  /// returns per entry and what every detail tool repeats at the top of its result, so an agent can
  /// tell which asset it is looking at without correlating against an earlier call.
  static void WriteAssetIdentity(plMcpJsonWriter& ref_writer, const plSubAsset& subAsset);

  /// How many finished transforms are kept. A full transform of a large project runs into the
  /// thousands, so this covers recent activity rather than the whole run.
  static constexpr plUInt32 s_uiMaxHistoryEntries = 500;

  /// Progress events arrive from the processor's worker threads, everything else runs on the main
  /// thread. Guards m_History and m_uiNextHistoryId.
  mutable plMutex m_HistoryMutex;
  plDeque<HistoryEntry> m_History;
  plEventSubscriptionID m_ProgressSubscription = 0;

  /// Assigned in order and never reused, so a 'sinceId' from an earlier call stays meaningful even
  /// once that entry has fallen out of the buffer. Same contract as log_read.
  plUInt64 m_uiNextHistoryId = 1;
};
