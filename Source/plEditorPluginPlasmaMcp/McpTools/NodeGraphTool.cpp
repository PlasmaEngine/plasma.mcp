#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpJson.h>
#include <plEditorPluginPlasmaMcp/McpJsonWriter.h>
#include <plEditorPluginPlasmaMcp/McpTools/NodeGraphTool.h>

#include <Foundation/Utilities/ConversionUtils.h>
#include <ToolsFoundation/Command/VisualGraphCommands.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/VisualGraph/VisualGraphObjectManager.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMcpNodeGraphTool, 1, plRTTIDefaultAllocator<plMcpNodeGraphTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  /// Resolves 'document' to an open document whose object manager is a node graph, or reports why not.
  plVisualGraphObjectManager* NodeToolFindManager(const plVariantDictionary& arguments, plDocument*& out_pDocument, plStringBuilder& out_sError)
  {
    const plStringView sDocument = plMcpJson::GetString(arguments, "document");

    if (sDocument.IsEmpty())
    {
      out_sError = "No 'document' argument given.";
      return nullptr;
    }

    out_pDocument = plMcpToolProvider::FindOpenDocument(sDocument);

    if (out_pDocument == nullptr)
    {
      out_sError.SetFormat("No open document matches '{}'. 'document_list' shows what is open; nothing is opened implicitly.", sDocument);
      return nullptr;
    }

    auto* pManager = dynamic_cast<plVisualGraphObjectManager*>(out_pDocument->GetObjectManager());

    if (pManager == nullptr)
    {
      out_sError.SetFormat("'{}' is not a node graph document, so it has no nodes, pins or connections.", out_pDocument->GetDocumentPath());
      return nullptr;
    }

    return pManager;
  }

  /// Object guids only - a node is not addressable by path, and accepting one would silently match nothing.
  const plDocumentObject* NodeToolFindObject(plDocument* pDocument, plStringView sGuid, plStringView sArgName, plStringBuilder& out_sError)
  {
    if (sGuid.IsEmpty())
    {
      out_sError.SetFormat("No '{}' argument given.", sArgName);
      return nullptr;
    }

    plUuid guid;
    if (!plConversionUtils::TryConvertStringToUuid(sGuid, guid))
    {
      out_sError.SetFormat("'{}' is not a valid object guid. Node guids come from 'node_graph' or 'object_tree'.", sGuid);
      return nullptr;
    }

    const plDocumentObject* pObject = pDocument->GetObjectManager()->GetObject(guid);

    if (pObject == nullptr)
    {
      out_sError.SetFormat("The document has no object with guid '{}'. Guids do not survive a reload; re-read the graph.", sGuid);
      return nullptr;
    }

    return pObject;
  }

  void WritePinArray(plMcpJsonWriter& writer, plStringView sName, plArrayPtr<const plUniquePtr<const plVisualGraphPin>> pins, const plVisualGraphObjectManager* pManager)
  {
    writer.BeginArray(sName);

    for (const auto& pPin : pins)
    {
      writer.BeginObject();
      writer.AddVariableString("name", pPin->GetName());
      writer.AddVariableBool("connected", pManager->HasConnections(*pPin));
      writer.EndObject();
    }

    writer.EndArray();
  }
} // namespace

plMcpNodeGraphTool::plMcpNodeGraphTool() = default;
plMcpNodeGraphTool::~plMcpNodeGraphTool() = default;

void plMcpNodeGraphTool::GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const
{
  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "node_pins";
    desc.m_sDescription = "Lists the input and output pin names of one node in a node graph document, and whether each is "
                          "already connected. Call this rather than guessing pin names from 'rtti_type_properties': a node "
                          "with a dynamic pin count exposes its pins as 'Name[0]', 'Name[1]' and so on, which the reflected "
                          "property list does not show.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string","description":"Guid or path of an open node graph document."},)"
                          R"("object":{"type":"string","description":"Guid of the node."})"
                          R"(},"required":["document","object"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "node_connect";
    desc.m_sDescription = "Connects an output pin of one node to an input pin of another, the same way dragging a wire in "
                          "the editor does: pin types are checked, cycles are refused, and the change is one undo step. "
                          "This is the only way to wire a graph - a connection is not a property, so 'object_modify' cannot "
                          "create one.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string","description":"Guid or path of an open node graph document."},)"
                          R"("source":{"type":"string","description":"Guid of the node the wire starts at."},)"
                          R"("sourcePin":{"type":"string","description":"Output pin name on the source node, from 'node_pins'."},)"
                          R"("target":{"type":"string","description":"Guid of the node the wire ends at."},)"
                          R"("targetPin":{"type":"string","description":"Input pin name on the target node, from 'node_pins'."})"
                          R"(},"required":["document","source","sourcePin","target","targetPin"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "node_disconnect";
    desc.m_sDescription = "Removes the connection between an output pin and an input pin. Reports whether a connection was "
                          "actually found.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string"},)"
                          R"("source":{"type":"string"},"sourcePin":{"type":"string"},)"
                          R"("target":{"type":"string"},"targetPin":{"type":"string"})"
                          R"(},"required":["document","source","sourcePin","target","targetPin"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "node_move";
    desc.m_sDescription = "Sets a node's position in the graph editor. Positions are node meta data rather than reflected "
                          "properties, so 'object_modify' cannot set them. Prefer 'node_layout' unless you want one "
                          "specific node somewhere.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string"},)"
                          R"("object":{"type":"string","description":"Guid of the node to move."},)"
                          R"("x":{"type":"number"},"y":{"type":"number"})"
                          R"(},"required":["document","object","x","y"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "node_graph";
    desc.m_sDescription = "Returns the whole graph: every node with its guid, type and position, and every connection with "
                          "its endpoints and pin names. This is the read-back for a graph an agent has built - 'object_tree' "
                          "shows the same objects but not which of them are connections nor where anything sits.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string"})"
                          R"(},"required":["document"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "node_layout";
    desc.m_sDescription = "Arranges every node in the graph automatically, so that a graph built through the tools is "
                          "readable instead of a pile at the origin. Nodes are placed in columns by how far they are from "
                          "an input along the wires, so data flows left to right, and are ordered within a column to sit "
                          "near the nodes they connect to. Returns the resulting node count and column count. One undo step.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string"},)"
                          R"("columnSpacing":{"type":"number","description":"Horizontal distance between columns. Defaults to 320."},)"
                          R"("rowSpacing":{"type":"number","description":"Vertical distance between nodes in a column. Defaults to 150."},)"
                          R"("originX":{"type":"number","description":"Position of the first column. Defaults to 0."},)"
                          R"("originY":{"type":"number","description":"Position of the first row. Defaults to 0."})"
                          R"(},"required":["document"]})";
  }
}

void plMcpNodeGraphTool::Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  if (sToolName == "node_pins")
    ExecutePins(arguments, out_result);
  else if (sToolName == "node_connect")
    ExecuteConnect(arguments, out_result);
  else if (sToolName == "node_disconnect")
    ExecuteDisconnect(arguments, out_result);
  else if (sToolName == "node_move")
    ExecuteMove(arguments, out_result);
  else if (sToolName == "node_graph")
    ExecuteRead(arguments, out_result);
  else if (sToolName == "node_layout")
    ExecuteLayout(arguments, out_result);
}

void plMcpNodeGraphTool::ExecutePins(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plDocument* pDocument = nullptr;
  plStringBuilder sError;
  auto* pManager = NodeToolFindManager(arguments, pDocument, sError);

  if (pManager == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  const plDocumentObject* pObject = NodeToolFindObject(pDocument, plMcpJson::GetString(arguments, "object"), "object", sError);

  if (pObject == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  if (!pManager->IsNode(pObject))
  {
    out_result.SetError("That object is not a node in this graph. Connections and embedded property objects have no pins.");
    return;
  }

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableString("object", plConversionUtils::ToString(pObject->GetGuid(), sError));
  writer.AddVariableString("type", pObject->GetType()->GetTypeName());
  WritePinArray(writer, "inputs", pManager->GetInputPins(pObject), pManager);
  WritePinArray(writer, "outputs", pManager->GetOutputPins(pObject), pManager);
  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpNodeGraphTool::ExecuteConnect(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plDocument* pDocument = nullptr;
  plStringBuilder sError;
  auto* pManager = NodeToolFindManager(arguments, pDocument, sError);

  if (pManager == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  const plDocumentObject* pSource = NodeToolFindObject(pDocument, plMcpJson::GetString(arguments, "source"), "source", sError);
  if (pSource == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  const plDocumentObject* pTarget = NodeToolFindObject(pDocument, plMcpJson::GetString(arguments, "target"), "target", sError);
  if (pTarget == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  const plStringView sSourcePin = plMcpJson::GetString(arguments, "sourcePin");
  const plStringView sTargetPin = plMcpJson::GetString(arguments, "targetPin");

  const plVisualGraphPin* pSourcePin = pManager->GetOutputPinByName(pSource, sSourcePin);
  if (pSourcePin == nullptr)
  {
    sError.SetFormat("'{}' has no output pin '{}'. 'node_pins' lists the ones it has.", pSource->GetType()->GetTypeName(), sSourcePin);
    out_result.SetError(sError);
    return;
  }

  const plVisualGraphPin* pTargetPin = pManager->GetInputPinByName(pTarget, sTargetPin);
  if (pTargetPin == nullptr)
  {
    sError.SetFormat("'{}' has no input pin '{}'. 'node_pins' lists the ones it has.", pTarget->GetType()->GetTypeName(), sTargetPin);
    out_result.SetError(sError);
    return;
  }

  // asked before starting a transaction, so that a refusal leaves nothing on the undo stack
  plVisualGraphObjectManager::CanConnectResult connectResult;
  const plStatus canConnect = pManager->CanConnect(pManager->GetConnectionType(), *pSourcePin, *pTargetPin, connectResult);

  if (canConnect.Failed())
  {
    sError.SetFormat("Cannot connect '{}' to '{}': {}", sSourcePin, sTargetPin, canConnect.m_sMessage);
    out_result.SetError(sError);
    return;
  }

  plCommandHistory* pHistory = pDocument->GetCommandHistory();
  pHistory->StartTransaction("MCP: connect nodes");

  const plStatus res = plNodeCommands::AddAndConnectCommand(pHistory, pManager->GetConnectionType(), *pSourcePin, *pTargetPin);

  if (res.Failed())
  {
    pHistory->CancelTransaction();
    sError.SetFormat("Connecting '{}' to '{}' failed: {}", sSourcePin, sTargetPin, res.m_sMessage);
    out_result.SetError(sError);
    return;
  }

  pHistory->FinishTransaction();

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableString("sourcePin", sSourcePin);
  writer.AddVariableString("targetPin", sTargetPin);
  writer.AddVariableBool("connected", true);
  writer.AddVariableString("note", "The document is not saved yet. Call 'document_save' to write it to disk.");
  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpNodeGraphTool::ExecuteDisconnect(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plDocument* pDocument = nullptr;
  plStringBuilder sError;
  auto* pManager = NodeToolFindManager(arguments, pDocument, sError);

  if (pManager == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  const plDocumentObject* pSource = NodeToolFindObject(pDocument, plMcpJson::GetString(arguments, "source"), "source", sError);
  const plDocumentObject* pTarget = NodeToolFindObject(pDocument, plMcpJson::GetString(arguments, "target"), "target", sError);

  if (pSource == nullptr || pTarget == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  const plStringView sSourcePin = plMcpJson::GetString(arguments, "sourcePin");
  const plStringView sTargetPin = plMcpJson::GetString(arguments, "targetPin");

  const plVisualGraphPin* pSourcePin = pManager->GetOutputPinByName(pSource, sSourcePin);
  const plVisualGraphPin* pTargetPin = pManager->GetInputPinByName(pTarget, sTargetPin);

  if (pSourcePin == nullptr || pTargetPin == nullptr)
  {
    out_result.SetError("One of the pins does not exist. 'node_pins' lists a node's pins.");
    return;
  }

  const plVisualGraphConnection* pConnection = nullptr;
  for (const plVisualGraphConnection* pCon : pManager->GetConnections(*pSourcePin))
  {
    if (&pCon->GetTargetPin() == pTargetPin)
    {
      pConnection = pCon;
      break;
    }
  }

  if (pConnection == nullptr)
  {
    out_result.m_sText = R"({"disconnected":false,"reason":"those pins are not connected"})";
    return;
  }

  plCommandHistory* pHistory = pDocument->GetCommandHistory();
  pHistory->StartTransaction("MCP: disconnect nodes");

  // A disconnected connection object cannot be serialized by the graph document. Match the
  // editor's wire-delete operation: disconnect AND remove it in the same undo transaction.
  const plStatus res = plNodeCommands::DisconnectAndRemoveCommand(pHistory, pConnection->GetParent()->GetGuid());

  if (res.Failed())
  {
    pHistory->CancelTransaction();
    out_result.SetError(res.m_sMessage);
    return;
  }

  pHistory->FinishTransaction();
  out_result.m_sText = R"({"disconnected":true,"note":"The document is not saved yet. Call 'document_save' to write it to disk."})";
}

void plMcpNodeGraphTool::ExecuteMove(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plDocument* pDocument = nullptr;
  plStringBuilder sError;
  auto* pManager = NodeToolFindManager(arguments, pDocument, sError);

  if (pManager == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  const plDocumentObject* pObject = NodeToolFindObject(pDocument, plMcpJson::GetString(arguments, "object"), "object", sError);

  if (pObject == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  if (!pManager->IsNode(pObject))
  {
    out_result.SetError("That object is not a node, so it has no position.");
    return;
  }

  const plVec2 vPos(static_cast<float>(plMcpJson::GetInt(arguments, "x", 0)), static_cast<float>(plMcpJson::GetInt(arguments, "y", 0)));

  plCommandHistory* pHistory = pDocument->GetCommandHistory();
  pHistory->StartTransaction("MCP: move node");

  plMoveNodeCommand cmd;
  cmd.m_Object = pObject->GetGuid();
  cmd.m_NewPos = vPos;

  const plStatus res = pHistory->AddCommand(cmd);

  if (res.Failed())
  {
    pHistory->CancelTransaction();
    out_result.SetError(res.m_sMessage);
    return;
  }

  pHistory->FinishTransaction();

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableFloat("x", vPos.x);
  writer.AddVariableFloat("y", vPos.y);
  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpNodeGraphTool::ExecuteRead(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plDocument* pDocument = nullptr;
  plStringBuilder sError;
  auto* pManager = NodeToolFindManager(arguments, pDocument, sError);

  if (pManager == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  plStringBuilder sGuid;
  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableString("document", pDocument->GetDocumentPath());

  writer.BeginArray("nodes");
  for (const plDocumentObject* pObject : pDocument->GetObjectManager()->GetRootObject()->GetChildren())
  {
    if (!pManager->IsNode(pObject))
      continue;

    const plVec2 vPos = pManager->GetNodePos(pObject);

    writer.BeginObject();
    writer.AddVariableString("guid", plConversionUtils::ToString(pObject->GetGuid(), sGuid));
    writer.AddVariableString("type", pObject->GetType()->GetTypeName());
    writer.AddVariableFloat("x", vPos.x);
    writer.AddVariableFloat("y", vPos.y);
    writer.EndObject();
  }
  writer.EndArray();

  writer.BeginArray("connections");
  for (const plDocumentObject* pObject : pDocument->GetObjectManager()->GetRootObject()->GetChildren())
  {
    if (!pManager->IsConnection(pObject))
      continue;

    const plVisualGraphConnection* pConnection = pManager->GetConnectionIfExists(pObject);

    if (pConnection == nullptr)
      continue;

    writer.BeginObject();
    writer.AddVariableString("source", plConversionUtils::ToString(pConnection->GetSourcePin().GetParent()->GetGuid(), sGuid));
    writer.AddVariableString("sourcePin", pConnection->GetSourcePin().GetName());
    writer.AddVariableString("target", plConversionUtils::ToString(pConnection->GetTargetPin().GetParent()->GetGuid(), sGuid));
    writer.AddVariableString("targetPin", pConnection->GetTargetPin().GetName());
    writer.EndObject();
  }
  writer.EndArray();

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpNodeGraphTool::ExecuteLayout(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plDocument* pDocument = nullptr;
  plStringBuilder sError;
  auto* pManager = NodeToolFindManager(arguments, pDocument, sError);

  if (pManager == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  const float fColumnSpacing = static_cast<float>(plMcpJson::GetInt(arguments, "columnSpacing", 320));
  const float fRowSpacing = static_cast<float>(plMcpJson::GetInt(arguments, "rowSpacing", 150));
  const float fOriginX = static_cast<float>(plMcpJson::GetInt(arguments, "originX", 0));
  const float fOriginY = static_cast<float>(plMcpJson::GetInt(arguments, "originY", 0));

  plHybridArray<const plDocumentObject*, 64> nodes;
  plHashTable<plUuid, plUInt32> nodeIndex;

  for (const plDocumentObject* pObject : pDocument->GetObjectManager()->GetRootObject()->GetChildren())
  {
    if (!pManager->IsNode(pObject))
      continue;

    nodeIndex[pObject->GetGuid()] = nodes.GetCount();
    nodes.PushBack(pObject);
  }

  if (nodes.IsEmpty())
  {
    out_result.m_sText = R"({"nodes":0,"columns":0})";
    return;
  }

  // edges as index pairs, so the layering below never has to touch the document again
  struct Edge
  {
    plUInt32 m_uiFrom = 0;
    plUInt32 m_uiTo = 0;
  };

  plHybridArray<Edge, 128> edges;

  for (const plDocumentObject* pObject : pDocument->GetObjectManager()->GetRootObject()->GetChildren())
  {
    if (!pManager->IsConnection(pObject))
      continue;

    const plVisualGraphConnection* pConnection = pManager->GetConnectionIfExists(pObject);

    if (pConnection == nullptr)
      continue;

    plUInt32 uiFrom = 0;
    plUInt32 uiTo = 0;

    if (nodeIndex.TryGetValue(pConnection->GetSourcePin().GetParent()->GetGuid(), uiFrom) &&
        nodeIndex.TryGetValue(pConnection->GetTargetPin().GetParent()->GetGuid(), uiTo))
    {
      Edge& edge = edges.ExpandAndGetRef();
      edge.m_uiFrom = uiFrom;
      edge.m_uiTo = uiTo;
    }
  }

  // Longest path layering: a node sits one column right of the furthest node feeding it, so every wire
  // points forwards and the column index reads as 'distance from an input'. Relaxed iteratively rather
  // than topologically sorted - the manager refuses cycles, so this settles in at most one pass per node,
  // and the bound keeps a graph that somehow does contain one from looping forever.
  plHybridArray<plUInt32, 64> column;
  column.SetCount(nodes.GetCount(), 0);

  for (plUInt32 uiPass = 0; uiPass < nodes.GetCount(); ++uiPass)
  {
    bool bChanged = false;

    for (const Edge& edge : edges)
    {
      if (column[edge.m_uiTo] < column[edge.m_uiFrom] + 1)
      {
        column[edge.m_uiTo] = column[edge.m_uiFrom] + 1;
        bChanged = true;
      }
    }

    if (!bChanged)
      break;
  }

  plUInt32 uiNumColumns = 0;
  for (plUInt32 c : column)
  {
    uiNumColumns = plMath::Max(uiNumColumns, c + 1);
  }

  plCommandHistory* pHistory = pDocument->GetCommandHistory();
  pHistory->StartTransaction("MCP: layout graph");

  plHybridArray<float, 64> assignedY;
  assignedY.SetCount(nodes.GetCount(), 0.0f);

  plHybridArray<plUInt32, 16> inColumn;

  for (plUInt32 uiColumn = 0; uiColumn < uiNumColumns; ++uiColumn)
  {
    inColumn.Clear();

    for (plUInt32 i = 0; i < nodes.GetCount(); ++i)
    {
      if (column[i] == uiColumn)
      {
        inColumn.PushBack(i);
      }
    }

    // Order a column by the average height of the nodes feeding it, so a node sits across from what it
    // reads and wires stay short. Columns are processed left to right, so those heights are already final.
    plHybridArray<float, 16> barycenter;
    barycenter.SetCount(inColumn.GetCount(), 0.0f);

    for (plUInt32 i = 0; i < inColumn.GetCount(); ++i)
    {
      float fSum = 0.0f;
      plUInt32 uiCount = 0;

      for (const Edge& edge : edges)
      {
        if (edge.m_uiTo == inColumn[i] && column[edge.m_uiFrom] < uiColumn)
        {
          fSum += assignedY[edge.m_uiFrom];
          ++uiCount;
        }
      }

      // nothing feeds it yet, so let it keep its natural order at the top of the column
      barycenter[i] = (uiCount > 0) ? (fSum / uiCount) : -1.0f;
    }

    // insertion sort: columns are small, and it keeps equal barycenters in creation order
    for (plUInt32 i = 1; i < inColumn.GetCount(); ++i)
    {
      for (plUInt32 j = i; j > 0 && barycenter[j] < barycenter[j - 1]; --j)
      {
        plMath::Swap(barycenter[j], barycenter[j - 1]);
        plMath::Swap(inColumn[j], inColumn[j - 1]);
      }
    }

    for (plUInt32 i = 0; i < inColumn.GetCount(); ++i)
    {
      const plUInt32 uiNode = inColumn[i];
      const plVec2 vPos(fOriginX + uiColumn * fColumnSpacing, fOriginY + i * fRowSpacing);

      assignedY[uiNode] = vPos.y;

      plMoveNodeCommand cmd;
      cmd.m_Object = nodes[uiNode]->GetGuid();
      cmd.m_NewPos = vPos;

      if (pHistory->AddCommand(cmd).Failed())
      {
        pHistory->CancelTransaction();
        out_result.SetError("Moving a node failed, the layout was not applied.");
        return;
      }
    }
  }

  pHistory->FinishTransaction();

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableUInt32("nodes", nodes.GetCount());
  writer.AddVariableUInt32("columns", uiNumColumns);
  writer.AddVariableUInt32("connections", edges.GetCount());
  writer.AddVariableString("note", "The document is not saved yet. Call 'document_save' to write it to disk.");
  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}
