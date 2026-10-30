#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpTools/McpToolRegistry.h>

plSet<const plRTTI*> plMcpToolRegistry::s_KnownTypes;
plDynamicArray<plMcpToolProvider*> plMcpToolRegistry::s_Providers;
plDynamicArray<plMcpToolDesc> plMcpToolRegistry::s_Tools;
plMap<plString, plMcpToolProvider*> plMcpToolRegistry::s_ToolLookup;

void plMcpToolRegistry::UpdateProviders()
{
  plRTTI::ForEachDerivedType<plMcpToolProvider>(
    [](const plRTTI* pRtti)
    {
      // remember the type even if it can't be allocated, so we don't look at it again
      if (s_KnownTypes.Contains(pRtti))
        return;

      s_KnownTypes.Insert(pRtti);

      if (!pRtti->GetAllocator()->CanAllocate())
        return;

      plMcpToolProvider* pProvider = pRtti->GetAllocator()->Allocate<plMcpToolProvider>();
      s_Providers.PushBack(pProvider);

      pProvider->OnActivate();

      plDynamicArray<plMcpToolDesc> tools;
      pProvider->GetSupportedTools(tools);

      for (const plMcpToolDesc& tool : tools)
      {
        if (s_ToolLookup.Contains(tool.m_sName))
        {
          // two providers claiming the same name would make dispatch ambiguous, and the client would
          // see a duplicate entry in its tool list
          plLog::Error("MCP: Tool name '{}' is already in use, the one from '{}' is ignored.", tool.m_sName, pRtti->GetTypeName());
          continue;
        }

        s_ToolLookup[tool.m_sName] = pProvider;
        s_Tools.PushBack(tool);
      }
    },
    plRTTI::ForEachOptions::ExcludeNotConcrete);
}

void plMcpToolRegistry::RemoveProvidersOfPlugin(plStringView sPluginBinary)
{
  for (plUInt32 i = s_Providers.GetCount(); i > 0; --i)
  {
    plMcpToolProvider* pProvider = s_Providers[i - 1];
    const plRTTI* pRtti = pProvider->GetDynamicRTTI();

    if (pRtti->GetPluginName() != sPluginBinary)
      continue;

    for (plUInt32 t = s_Tools.GetCount(); t > 0; --t)
    {
      auto it = s_ToolLookup.Find(s_Tools[t - 1].m_sName);
      if (it.IsValid() && it.Value() == pProvider)
      {
        s_ToolLookup.Remove(it);
        s_Tools.RemoveAtAndCopy(t - 1);
      }
    }

    pProvider->OnDeactivate();
    pRtti->GetAllocator()->Deallocate(pProvider);
    s_Providers.RemoveAtAndCopy(i - 1);
  }

  // the RTTI objects die with the plugin, and a later type could be allocated at the same address
  for (auto it = s_KnownTypes.GetIterator(); it.IsValid();)
  {
    if (it.Key()->GetPluginName() == sPluginBinary)
      it = s_KnownTypes.Remove(it);
    else
      ++it;
  }
}

void plMcpToolRegistry::Clear()
{
  for (plMcpToolProvider* pProvider : s_Providers)
  {
    pProvider->OnDeactivate();
    pProvider->GetDynamicRTTI()->GetAllocator()->Deallocate(pProvider);
  }

  s_Providers.Clear();
  s_Tools.Clear();
  s_ToolLookup.Clear();
  s_KnownTypes.Clear();
}

plResult plMcpToolRegistry::Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  auto it = s_ToolLookup.Find(sToolName);

  if (!it.IsValid())
    return PL_FAILURE;

  it.Value()->Execute(sToolName, arguments, out_result);
  return PL_SUCCESS;
}
