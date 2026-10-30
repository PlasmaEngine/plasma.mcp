#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpJson.h>
#include <plEditorPluginPlasmaMcp/McpJsonWriter.h>
#include <plEditorPluginPlasmaMcp/McpTools/ActionTool.h>

#include <Foundation/Strings/TranslationLookup.h>

#include <GuiFoundation/Action/ActionManager.h>
#include <GuiFoundation/Action/BaseActions.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMcpActionTool, 1, plRTTIDefaultAllocator<plMcpActionTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  constexpr plUInt32 uiMaxActions = 200;

  plStringView ToString(plActionScope::Enum scope)
  {
    switch (scope)
    {
      case plActionScope::Global:
        return "Global";
      case plActionScope::Document:
        return "Document";
      case plActionScope::Window:
        return "Window";
    }

    return "Unknown";
  }

  plStringView ToString(plActionType::Enum type)
  {
    switch (type)
    {
      case plActionType::Action:
        return "Action";
      case plActionType::Category:
        return "Category";
      case plActionType::Menu:
        return "Menu";
      case plActionType::ActionAndMenu:
        return "ActionAndMenu";
    }

    return "Unknown";
  }

  /// \brief Returns a live instance of a global action, or nullptr.
  ///
  /// Global actions exist once per window they are mapped into, and all of those agree, so the first one
  /// is as good as any. For other scopes the instances belong to different documents and would each
  /// report something different, so none of them is picked.
  ///
  /// Reading an existing instance rather than creating one matters twice over: creating an action has
  /// side effects (it registers event handlers, and some constructors touch the editor), and a fresh
  /// instance would not have received the updates that keep m_bEnabled current.
  const plAction* GetGlobalInstance(const plActionDescriptor& desc)
  {
    if (desc.m_Scope != plActionScope::Global)
      return nullptr;

    const plArrayPtr<plAction* const> instances = desc.GetCreatedActions();

    return instances.IsEmpty() ? nullptr : instances[0];
  }

  /// \brief The live instance of a document action belonging to one document, or nullptr.
  ///
  /// Instances of a document action exist per document, so unlike the global case there is no
  /// 'any of them will do' - the one for the wrong document reports the wrong state and, executed,
  /// acts on the wrong scene.
  plAction* GetDocumentInstance(const plActionDescriptor& desc, const plDocument* pDocument)
  {
    if (desc.m_Scope != plActionScope::Document || pDocument == nullptr)
      return nullptr;

    for (plAction* pAction : desc.GetCreatedActions())
    {
      if (pAction != nullptr && pAction->GetContext().m_pDocument == pDocument)
        return pAction;
    }

    return nullptr;
  }

  /// \brief Writes 'enabled', and 'checkable'/'checked' where they apply.
  ///
  /// Everything that cannot be determined is written as the string "unknown" rather than omitted, so
  /// that a caller can tell 'not applicable or not known' apart from 'false'. That happens for
  /// non-global actions, for global ones that are not currently mapped into any window, and for action
  /// types that carry no enabled flag at all (categories and plain menus).
  void WriteActionState(plMcpJsonWriter& ref_writer, const plActionDescriptor& desc, const plDocument* pDocument = nullptr)
  {
    const plAction* pAction = pDocument != nullptr ? GetDocumentInstance(desc, pDocument) : GetGlobalInstance(desc);

    if (pAction == nullptr)
    {
      ref_writer.AddVariableString("enabled", "unknown");

      if (desc.m_Scope == plActionScope::Document)
      {
        ref_writer.AddVariableString("stateReason", pDocument != nullptr
                                                      ? "This document has no instance of the action, so it does not apply to it."
                                                      : "A document action has a state only for a specific document. Pass 'document'.");
      }
      else if (desc.m_Scope != plActionScope::Global)
      {
        ref_writer.AddVariableString("stateReason", "Only global actions have a state that can be read without a document context.");
      }
      else
      {
        ref_writer.AddVariableString("stateReason", "No instance of this action exists right now, so it is not mapped into any open window.");
      }

      return;
    }

    if (const plButtonAction* pButton = plDynamicCast<const plButtonAction*>(pAction))
    {
      ref_writer.AddVariableBool("enabled", pButton->IsEnabled());
      ref_writer.AddVariableBool("visible", pButton->IsVisible());

      // only meaningful for toggles, and reporting 'checked:false' for every push button would suggest
      // a state that does not exist
      if (pButton->IsCheckable())
      {
        ref_writer.AddVariableBool("checkable", true);
        ref_writer.AddVariableBool("checked", pButton->IsChecked());
      }
    }
    else if (const plDynamicActionAndMenuAction* pMenu = plDynamicCast<const plDynamicActionAndMenuAction*>(pAction))
    {
      ref_writer.AddVariableBool("enabled", pMenu->IsEnabled());
      ref_writer.AddVariableBool("visible", pMenu->IsVisible());
    }
    else if (const plSliderAction* pSlider = plDynamicCast<const plSliderAction*>(pAction))
    {
      ref_writer.AddVariableBool("enabled", pSlider->IsEnabled());
      ref_writer.AddVariableBool("visible", pSlider->IsVisible());
      ref_writer.AddVariableInt32("value", pSlider->GetValue());

      plInt32 iMin = 0, iMax = 0;
      pSlider->GetRange(iMin, iMax);
      ref_writer.AddVariableInt32("minValue", iMin);
      ref_writer.AddVariableInt32("maxValue", iMax);
    }
    else
    {
      ref_writer.AddVariableString("enabled", "unknown");
      ref_writer.AddVariableString("stateReason", "This action type has no enabled state.");
    }
  }

  /// \brief Writes the strings a user would see for this action, skipping those that translate to nothing.
  ///
  /// All three lookups are keyed on the action name and fall back to returning that name unchanged when
  /// there is no entry, which is why the tooltip and help URL are compared against it before being
  /// written - otherwise every action without a tooltip would appear to have its own name as one.
  void WriteTranslations(plMcpJsonWriter& ref_writer, const plActionDescriptor& desc)
  {
    const plStringView sName = desc.m_sActionName;

    const plStringView sDisplay = plTranslate(sName.GetStartPointer());

    if (!sDisplay.IsEmpty() && sDisplay != sName)
    {
      ref_writer.AddVariableString("displayName", sDisplay);
    }

    const plStringView sTooltip = plTranslateTooltip(sName.GetStartPointer());

    if (!sTooltip.IsEmpty() && sTooltip != sName)
    {
      ref_writer.AddVariableString("tooltip", sTooltip);
    }

    const plStringView sHelpUrl = plTranslateHelpURL(sName.GetStartPointer());

    if (!sHelpUrl.IsEmpty() && sHelpUrl != sName)
    {
      ref_writer.AddVariableString("helpUrl", sHelpUrl);
    }
  }
} // namespace

void plMcpActionTool::GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const
{
  plMcpToolDesc& list = out_tools.ExpandAndGetRef();
  list.m_sName = "action_list";
  list.m_sDescription = "Lists the editor's actions - the commands behind its menu entries and toolbar buttons - with their "
                        "internal name, category, keyboard shortcut and the text the user sees for them. Use it to find out "
                        "what the editor can do and what to pass to action_execute, or to explain a feature to a user in the "
                        "wording that actually appears in their UI. Scope 'Global' actions are executable as they are; scope "
                        "'Document' ones need the 'document' argument of action_execute and only work on a document whose "
                        "window is open. Scope 'Window' ones cannot be executed at all.";
  list.m_sInputSchema = R"({"type":"object","properties":{)"
                        R"("contains":{"type":"string","description":"Only list actions whose internal name or category contains this text, case insensitive."},)"
                        R"("matchTranslation":{"type":"boolean","description":"Also match 'contains' against the translated display name and tooltip, i.e. the words the user sees. Useful when searching for a feature by how it is described rather than by its internal name. Default false."},)"
                        R"("globalOnly":{"type":"boolean","description":"Only list actions of scope 'Global'. Default true. Set to false to also see the 'Document' scope actions, which action_execute can run when given a document - this is where scene play/simulate and most per-asset commands live."},)"
                        R"("includeMenus":{"type":"boolean","description":"Also list menus, which only group other entries and cannot be executed. Default false. Turn this on to describe the structure of the editor's menu bar to a user, not to find something to run."},)"
                        R"("includeState":{"type":"boolean","description":"Include the current enabled/checked state of each action. Default false, because it makes the result considerably longer."}}})";

  plMcpToolDesc& state = out_tools.ExpandAndGetRef();
  state.m_sName = "action_state";
  state.m_sDescription = "Reports whether one action is currently enabled, and whether it is checked if it is a toggle. Read "
                         "from the live action as the UI shows it, so it is accurate at the moment of the call - but it is "
                         "only a hint, since anything that happens afterwards can change it. 'enabled' comes back as the "
                         "string \"unknown\" when it cannot be determined, which is the case for actions that are not global "
                         "and for those not currently present in any open window.";
  state.m_sInputSchema = R"({"type":"object","properties":{)"
                         R"("name":{"type":"string","description":"The internal action name, e.g. 'Document.Save' or 'Editor.Project.Settings'. Get it from action_list."},)"
                         R"("category":{"type":"string","description":"The category the action is registered in. Only needed when the same name exists in several categories, which action_list reports."},)"
                         R"("document":{"type":"string","description":"Guid or path of an open document, for a scope 'Document' action. Its state is per document - this is how to find out whether a scene is currently playing, by reading 'Scene.GameMode.Stop'."}},)"
                         R"("required":["name"]})";

  plMcpToolDesc& exec = out_tools.ExpandAndGetRef();
  exec.m_sName = "action_execute";
  exec.m_sDescription = "Triggers one action, exactly as clicking its menu entry or toolbar button would. Refuses and "
                        "reports 'wasDisabled' if the action is currently disabled, rather than invoking something that the "
                        "UI would not let a user invoke.\n"
                        "Scope 'Global' actions need nothing else. Scope 'Document' actions need 'document', and only work on "
                        "a document whose window is open - this is how scene play and simulate are reached, through "
                        "'Scene.GameMode.Play', 'Scene.GameMode.Simulate', 'Scene.GameMode.Stop' and 'Scene.GameMode.Pause'. "
                        "Scope 'Window' actions are rejected.\n"
                        "IMPORTANT - this call can hang and there is no way to cancel it. It runs synchronously, and an action "
                        "that opens a modal dialog blocks the editor's main thread until a human closes that dialog, which in "
                        "an automated setting never happens. The editor is then stuck: this call never returns, app_quit "
                        "cannot get through either, and the only way out is killing the process. So: always call this with a "
                        "timeout of about 30 seconds rather than waiting indefinitely, know the process id from app_info or "
                        "app_ping beforehand, and if the timeout expires use app_ping to confirm the editor is unresponsive "
                        "and then kill it by process id. Actions that open dialogs cannot be identified in advance, so treat "
                        "any unfamiliar action as capable of this - a name containing words like 'Settings', 'Options', "
                        "'Export', 'Dialog' or 'About' is a strong hint.";
  exec.m_sInputSchema = R"({"type":"object","properties":{)"
                        R"("name":{"type":"string","description":"The internal action name, as reported by action_list."},)"
                        R"("category":{"type":"string","description":"The category the action is registered in. Only needed to disambiguate a name that exists in several categories."},)"
                        R"("document":{"type":"string","description":"Guid or path of the open document to act on. Required for scope 'Document' actions, ignored for global ones. The document's window must be open, since the action instance belongs to it."},)"
                        R"("value":{"description":"Optional value passed to the action. Most actions ignore it; sliders and toggles use it."},)"
                        R"("force":{"type":"boolean","description":"Execute even when the action reports itself as disabled. Default false. The action may misbehave, since it is being invoked in a state its UI would prevent."}},)"
                        R"("required":["name"]})";
}

void plMcpActionTool::Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  if (sToolName == "action_list")
  {
    ExecuteList(arguments, out_result);
  }
  else if (sToolName == "action_state")
  {
    ExecuteState(arguments, out_result);
  }
  else if (sToolName == "action_execute")
  {
    ExecuteAction(arguments, out_result);
  }
}

const plActionDescriptor* plMcpActionTool::FindAction(plStringView sName, plStringView sCategory, bool& out_bAmbiguous)
{
  out_bAmbiguous = false;

  const plActionDescriptor* pFound = nullptr;

  for (auto it = plActionManager::GetActionIterator(); it.IsValid(); ++it)
  {
    const plActionDescriptor* pDesc = it.Value();

    if (pDesc->m_sActionName != sName)
      continue;

    if (!sCategory.IsEmpty() && pDesc->m_sCategoryPath != sCategory)
      continue;

    if (pFound != nullptr)
    {
      // a name registered in several categories - reported rather than silently resolved, because the
      // two may do entirely different things
      out_bAmbiguous = true;
      return nullptr;
    }

    pFound = pDesc;
  }

  return pFound;
}

void plMcpActionTool::ExecuteList(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sContains = plMcpJson::GetString(arguments, "contains");
  const bool bMatchTranslation = plMcpJson::GetBool(arguments, "matchTranslation", false);
  const bool bGlobalOnly = plMcpJson::GetBool(arguments, "globalOnly", true);
  const bool bIncludeMenus = plMcpJson::GetBool(arguments, "includeMenus", false);
  const bool bIncludeState = plMcpJson::GetBool(arguments, "includeState", false);

  plUInt32 uiTotalMatches = 0;
  plUInt32 uiReturned = 0;

  plMcpJsonWriter writer;
  writer.BeginObject();

  writer.BeginArray("actions");

  for (auto it = plActionManager::GetActionIterator(); it.IsValid(); ++it)
  {
    const plActionDescriptor* pDesc = it.Value();

    if (bGlobalOnly && pDesc->m_Scope != plActionScope::Global)
      continue;

    // categories are pure grouping for the shortcut dialog, they are never executable and would only
    // pad the list
    if (pDesc->m_Type == plActionType::Category)
      continue;

    // Menus are half of all global entries and none of them can be executed, so they are left out
    // unless asked for. 'ActionAndMenu' is kept either way - it is a button that also has a menu, and
    // the button part does work.
    if (!bIncludeMenus && pDesc->m_Type == plActionType::Menu)
      continue;

    if (!sContains.IsEmpty())
    {
      bool bMatches = pDesc->m_sActionName.FindSubString_NoCase(sContains) != nullptr ||
                      pDesc->m_sCategoryPath.FindSubString_NoCase(sContains) != nullptr;

      if (!bMatches && bMatchTranslation)
      {
        const plStringView sName = pDesc->m_sActionName;
        const plStringBuilder sDisplay = plTranslate(sName.GetStartPointer());
        const plStringBuilder sTooltip = plTranslateTooltip(sName.GetStartPointer());

        bMatches = sDisplay.FindSubString_NoCase(sContains) != nullptr || sTooltip.FindSubString_NoCase(sContains) != nullptr;
      }

      if (!bMatches)
        continue;
    }

    ++uiTotalMatches;

    if (uiReturned >= uiMaxActions)
      continue;

    writer.BeginObject();

    writer.AddVariableString("name", pDesc->m_sActionName);

    if (!pDesc->m_sCategoryPath.IsEmpty())
    {
      writer.AddVariableString("category", pDesc->m_sCategoryPath);
    }

    writer.AddVariableString("scope", ToString(pDesc->m_Scope));

    // 'Action' is the overwhelming majority, so only the exceptions are worth the tokens
    if (pDesc->m_Type != plActionType::Action)
    {
      writer.AddVariableString("type", ToString(pDesc->m_Type));
    }

    if (!pDesc->m_sShortcut.IsEmpty())
    {
      writer.AddVariableString("shortcut", pDesc->m_sShortcut);
    }

    WriteTranslations(writer, *pDesc);

    if (bIncludeState)
    {
      WriteActionState(writer, *pDesc);
    }

    writer.EndObject();

    ++uiReturned;
  }

  writer.EndArray();

  writer.AddVariableUInt32("totalMatches", uiTotalMatches);
  writer.AddVariableUInt32("returned", uiReturned);
  writer.AddVariableBool("truncated", uiTotalMatches > uiReturned);

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpActionTool::ExecuteState(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sName = plMcpJson::GetString(arguments, "name");
  const plStringView sCategory = plMcpJson::GetString(arguments, "category");

  if (sName.IsEmpty())
  {
    out_result.SetError("No action name given. Call action_list to find one.");
    return;
  }

  bool bAmbiguous = false;
  const plActionDescriptor* pDesc = FindAction(sName, sCategory, bAmbiguous);

  if (pDesc == nullptr)
  {
    plStringBuilder sMsg;

    if (bAmbiguous)
    {
      sMsg.SetFormat("The action '{}' exists in more than one category. Pass 'category' as well, see action_list.", sName);
    }
    else
    {
      sMsg.SetFormat("No action named '{}'. Call action_list to find the correct name.", sName);
    }

    out_result.SetError(sMsg);
    return;
  }

  // A document action's state is per document, so without one there is nothing to read. Resolving it
  // to nullptr is fine: WriteActionState() then says why the state is unknown.
  const plDocument* pDocument = plMcpToolProvider::FindOpenDocument(plMcpJson::GetString(arguments, "document"));

  plMcpJsonWriter writer;
  writer.BeginObject();

  writer.AddVariableString("name", pDesc->m_sActionName);
  writer.AddVariableString("category", pDesc->m_sCategoryPath);
  writer.AddVariableString("scope", ToString(pDesc->m_Scope));

  WriteActionState(writer, *pDesc, pDocument);
  WriteTranslations(writer, *pDesc);

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpActionTool::ExecuteAction(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sName = plMcpJson::GetString(arguments, "name");
  const plStringView sCategory = plMcpJson::GetString(arguments, "category");
  const bool bForce = plMcpJson::GetBool(arguments, "force", false);

  if (sName.IsEmpty())
  {
    out_result.SetError("No action name given. Call action_list to find one.");
    return;
  }

  bool bAmbiguous = false;
  const plActionDescriptor* pDesc = FindAction(sName, sCategory, bAmbiguous);

  if (pDesc == nullptr)
  {
    plStringBuilder sMsg;

    if (bAmbiguous)
    {
      sMsg.SetFormat("The action '{}' exists in more than one category. Pass 'category' as well, see action_list.", sName);
    }
    else
    {
      sMsg.SetFormat("No action named '{}'. Call action_list to find the correct name.", sName);
    }

    out_result.SetError(sMsg);
    return;
  }

  const plStringView sDocument = plMcpJson::GetString(arguments, "document");

  if (pDesc->m_Scope == plActionScope::Window)
  {
    plStringBuilder sMsg;
    sMsg.SetFormat("'{}' has scope 'Window'. It acts on a specific editor window, which this tool cannot identify.", sName);

    out_result.SetError(sMsg);
    return;
  }

  if (pDesc->m_Scope == plActionScope::Document && sDocument.IsEmpty())
  {
    plStringBuilder sMsg;
    sMsg.SetFormat("'{}' has scope 'Document', so it needs a 'document' argument naming the open document to act on. "
                   "'document_list' reports what is open.",
      sName);

    out_result.SetError(sMsg);
    return;
  }

  // categories and plain menus have an Execute() that does nothing, so calling them would report
  // success while achieving nothing at all
  if (pDesc->m_Type == plActionType::Category || pDesc->m_Type == plActionType::Menu)
  {
    plStringBuilder sMsg;
    sMsg.SetFormat("'{}' is a '{}', which only groups other actions and does nothing when triggered.", sName, ToString(pDesc->m_Type));

    out_result.SetError(sMsg);
    return;
  }

  // A document action is executed through the instance the editor's own UI already created for that
  // document, never through a fresh one. Their constructors cast plActionContext::m_pDocument to the
  // type they belong to without checking it - plSceneAction does, and dereferences it immediately -
  // so handing a document of the wrong type to CreateAction() is undefined behaviour, and nothing in
  // the descriptor says which type an action wants. An instance the UI built is by definition
  // attached to a document the action accepts.
  plAction* pExistingDocumentAction = nullptr;

  if (pDesc->m_Scope == plActionScope::Document)
  {
    plDocument* pDocument = plMcpToolProvider::FindOpenDocument(sDocument);

    if (pDocument == nullptr)
    {
      plStringBuilder sMsg;
      sMsg.SetFormat("No open document matches '{}'. Documents are never opened implicitly; use 'document_open' first.", sDocument);
      out_result.SetError(sMsg);
      return;
    }

    for (plAction* pAction : pDesc->GetCreatedActions())
    {
      if (pAction != nullptr && pAction->GetContext().m_pDocument == pDocument)
      {
        pExistingDocumentAction = pAction;
        break;
      }
    }

    if (pExistingDocumentAction == nullptr)
    {
      plStringBuilder sMsg;
      sMsg.SetFormat("'{}' is not available for that document - the editor has created no instance of it there. Either the action "
                     "belongs to a different document type, or that document's window is not open.",
        sName);
      out_result.SetError(sMsg);
      return;
    }
  }

  // Checked against a live instance before doing anything, because plActionManager::ExecuteAction()
  // does not: it creates an action and calls Execute() on it regardless of the enabled flag. Invoking a
  // disabled action is what the UI exists to prevent, and for some actions it would assert or act on
  // state that isn't there.
  if (const plAction* pExisting = pExistingDocumentAction != nullptr ? pExistingDocumentAction : GetGlobalInstance(*pDesc))
  {
    bool bEnabled = true;

    if (const plButtonAction* pButton = plDynamicCast<const plButtonAction*>(pExisting))
    {
      bEnabled = pButton->IsEnabled();
    }
    else if (const plDynamicActionAndMenuAction* pMenu = plDynamicCast<const plDynamicActionAndMenuAction*>(pExisting))
    {
      bEnabled = pMenu->IsEnabled();
    }
    else if (const plSliderAction* pSlider = plDynamicCast<const plSliderAction*>(pExisting))
    {
      bEnabled = pSlider->IsEnabled();
    }

    if (!bEnabled && !bForce)
    {
      plMcpJsonWriter writer;
      writer.BeginObject();
      writer.AddVariableBool("executed", false);
      writer.AddVariableBool("wasDisabled", true);
      writer.AddVariableString("reason",
        "The action is currently disabled, so it was not executed. Whatever it needs is not available right now - for many "
        "actions that means no document is open or nothing is selected. Pass force=true to run it anyway.");
      writer.EndObject();

      // an error, so that this cannot be mistaken for the action having run
      out_result.m_sText = writer.GetResult();
      out_result.m_bIsError = true;
      return;
    }
  }

  const plVariant* pValue = nullptr;
  arguments.TryGetValue("value", pValue);

  // A document action reuses the editor's instance and must not be destroyed afterwards - it belongs
  // to the window that made it. A global action gets a fresh one, whose context is deliberately empty
  // because global actions do not act on a document.
  plAction* pAction = pExistingDocumentAction;
  const bool bOwnsAction = pAction == nullptr;

  if (bOwnsAction)
  {
    plActionContext context;
    pAction = pDesc->CreateAction(context);
  }

  if (pAction == nullptr)
  {
    out_result.SetError(plStringBuilder("Failed to create the action '", sName, "'."));
    return;
  }

  // Blocking, and there is nothing to be done about it here: an action that opens a modal dialog runs a
  // nested event loop and will not come back until it is closed, which no one is there to do. The whole
  // editor is then stuck - this response is never sent, and no other tool call is served either,
  // because they all run on this thread.
  //
  // Guarding against it is not possible: whether an action shows a dialog is only known once it does.
  // Running it on another thread is not an option either, since actions touch the UI. So the tool
  // description tells callers to use a timeout and to kill the process by id when it expires, and
  // app_ping exists to confirm that state from the outside.
  pAction->Execute(pValue != nullptr ? *pValue : plVariant());

  if (bOwnsAction)
    pDesc->DeleteAction(pAction);

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableBool("executed", true);
  writer.AddVariableString("name", pDesc->m_sActionName);

  // Actions report nothing back - plAction::Execute() returns void - so there is no result to pass on
  // and 'executed' only means that it was invoked without anything going wrong on the way there. What
  // it did has to be observed elsewhere, e.g. through log_read.
  writer.AddVariableString("note", "Actions do not return a result. Check log_read or query the editor state to see what happened.");

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}
