#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

#include <Foundation/Utilities/ConversionUtils.h>
#include <ToolsFoundation/Document/DocumentManager.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMcpToolProvider, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plDocument* plMcpToolProvider::FindOpenDocument(plStringView sIdentifier)
{
  if (sIdentifier.IsEmpty())
    return nullptr;

  // TryConvertStringToUuid() rather than ConvertStringToUuid(), which asserts on anything that is
  // not a uuid - and the identifier here is a path just as often as a guid.
  if (plUuid guid; plConversionUtils::TryConvertStringToUuid(sIdentifier, guid))
    return plDocumentManager::GetDocumentByGuid(guid);

  plStringBuilder sPath = sIdentifier;
  sPath.MakeCleanPath();

  for (plDocumentManager* pManager : plDocumentManager::GetAllDocumentManagers())
  {
    if (plPathUtils::IsAbsolutePath(sPath))
    {
      if (plDocument* pDoc = pManager->GetDocumentByPath(sPath))
        return pDoc;

      continue;
    }

    for (plDocument* pDoc : pManager->GetAllOpenDocuments())
    {
      if (pDoc == nullptr)
        continue;

      plStringBuilder sDocPath = pDoc->GetDocumentPath();
      sDocPath.MakeCleanPath();

      if (sDocPath.EndsWith_NoCase(sPath))
      {
        // Only on a path separator, so that "Barrel.plScene" does not match "BigBarrel.plScene".
        const plUInt32 uiDocLen = sDocPath.GetElementCount();
        const plUInt32 uiPathLen = sPath.GetElementCount();

        if (uiDocLen == uiPathLen || sDocPath.GetData()[uiDocLen - uiPathLen - 1] == '/')
          return pDoc;
      }
    }
  }

  return nullptr;
}
