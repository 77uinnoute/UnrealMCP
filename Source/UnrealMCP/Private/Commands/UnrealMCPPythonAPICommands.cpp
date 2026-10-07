#include "Commands/UnrealMCPPythonAPICommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Core/MCPCommandRegistry.h"
#include "IPythonScriptPlugin.h"

namespace
{
    // The generated snippet prints one line prefixed with this marker; the bridge picks that line
    // out of the python log. A marker (instead of "parse the last print") survives snippets that
    // print their own diagnostics.
    const TCHAR* GPythonJsonMarker = TEXT("@@MCPJSON@@");

    // Shared prologue: the JSON emitter and the two helpers the snippets lean on.
    //
    // `signature` is derived from the first docstring line on purpose: these bindings are native
    // (inspect.signature raises on them), and the UE python binding convention puts the call shape
    // ("x.set_preview_mesh(mesh: SkeletalMesh) -> None") on that line.
    FString MakeIntrospectionPrologue()
    {
        return FString(TEXT(
            "import json, unreal\n"
            "def _emit(payload):\n"
            "    print('@@MCPJSON@@' + json.dumps(payload, default=str))\n"
            "def _sig(doc):\n"
            "    lines = (doc or '').strip().splitlines()\n"
            "    return lines[0].strip() if lines else ''\n"
            "def _module(cls):\n"
            "    try:\n"
            "        return unreal.SystemLibrary.get_path_name(cls.static_class())\n"
            "    except Exception:\n"
            "        return ''\n"
            "def _candidates(container, needle):\n"
            "    low = needle.lower()\n"
            "    return sorted([n for n in dir(container) if low in n.lower()])[:20]\n"));
    }
}

FUnrealMCPPythonAPICommands::FUnrealMCPPythonAPICommands()
{
}

bool FUnrealMCPPythonAPICommands::IsSafeIdentifier(const FString& Text)
{
    if (Text.IsEmpty())
    {
        return false;
    }
    for (int32 Index = 0; Index < Text.Len(); ++Index)
    {
        const TCHAR Character = Text[Index];
        const bool bLetterOrDigit = FChar::IsAlpha(Character) || FChar::IsDigit(Character);
        if (!(bLetterOrDigit || Character == TEXT('_')))
        {
            return false;
        }
    }
    return !FChar::IsDigit(Text[0]);
}

bool FUnrealMCPPythonAPICommands::RunIntrospectionPython(const FString& PyCode, TSharedPtr<FJsonObject>& OutPayload,
                                                         TSharedPtr<FJsonObject>& OutError)
{
    IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
    if (!(PythonPlugin && PythonPlugin->IsPythonAvailable()))
    {
        OutError = FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("python_unavailable: the editor python plugin is not available in this editor"));
        return false;
    }

    FPythonCommandEx Command;
    Command.Command = PyCode;
    Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
    PythonPlugin->ExecPythonCommandEx(Command);

    FString PayloadText;
    for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
    {
        FString Output = Entry.Output;
        Output.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
        const int32 MarkerIndex = Output.Find(GPythonJsonMarker, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (MarkerIndex != INDEX_NONE)
        {
            PayloadText = Output.Mid(MarkerIndex + FCString::Strlen(GPythonJsonMarker));
        }
    }

    if (!PayloadText.IsEmpty())
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PayloadText);
        if (FJsonSerializer::Deserialize(Reader, OutPayload) && OutPayload.IsValid())
        {
            return true;
        }
        OutError = FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("bad_payload: introspection output was not JSON: %s"), *PayloadText.Left(200)));
        return false;
    }

    // Nothing printed: the snippet raised, so pass the traceback through instead of a generic
    // "failed" (the whole point of this command family is to stop guessing).
    FString Traceback;
    for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
    {
        if (Entry.Type == EPythonLogOutputType::Error)
        {
            Traceback += Entry.Output;
        }
    }
    if (Traceback.IsEmpty())
    {
        Traceback = Command.CommandResult;
    }
    Traceback.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
    Traceback.TrimStartAndEndInline();
    if (Traceback.IsEmpty() || Traceback == TEXT("None"))
    {
        Traceback = TEXT("no output produced");
    }
    OutError = FUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("python_error: %s"), *Traceback.Left(1000)));
    return false;
}

TSharedPtr<FJsonObject> FUnrealMCPPythonAPICommands::HandlePythonAPIIndex(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassName;
    if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'class_name' parameter"));
    }
    if (!IsSafeIdentifier(ClassName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("invalid_identifier: '%s' (pass a python class name, e.g. 'IKRetargeterController')"), *ClassName));
    }

    const FString PyCode = MakeIntrospectionPrologue() + FString::Printf(TEXT(
        "name = \"%s\"\n"
        "cls = getattr(unreal, name, None)\n"
        "if cls is None:\n"
        "    _emit({'found': False, 'class_name': name, 'candidates': _candidates(unreal, name)})\n"
        "else:\n"
        "    funcs = []\n"
        "    for attr in sorted(dir(cls)):\n"
        "        if attr.startswith('_'):\n"
        "            continue\n"
        "        doc = getattr(getattr(cls, attr, None), '__doc__', None) or ''\n"
        "        if not doc:\n"
        "            continue\n"
        "        funcs.append({'function': attr, 'signature': _sig(doc), 'doc': doc})\n"
        "    _emit({'found': True, 'class_name': name, 'module': _module(cls), 'count': len(funcs), 'functions': funcs})\n"),
        *ClassName);

    TSharedPtr<FJsonObject> Payload;
    TSharedPtr<FJsonObject> Error;
    if (!RunIntrospectionPython(PyCode, Payload, Error))
    {
        return Error;
    }

    bool bFound = false;
    Payload->TryGetBoolField(TEXT("found"), bFound);
    if (!bFound)
    {
        Payload->SetBoolField(TEXT("success"), false);
        Payload->SetStringField(TEXT("error"), TEXT("unknown_class"));
        Payload->SetStringField(TEXT("hint"), TEXT("pass a class exposed by the editor python 'unreal' module"));
        return Payload;
    }

    Payload->SetBoolField(TEXT("success"), true);
    return Payload;
}

TSharedPtr<FJsonObject> FUnrealMCPPythonAPICommands::HandlePythonAPIDoc(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassName;
    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'class_name' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("function"), FunctionName) || FunctionName.IsEmpty())
    {
        // The name of this parameter is a measured trap (callers reach for `function_name`), so the
        // error says which name it is AND what a correct call looks like.
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("missing_parameter: 'function' is required (the member name parameter is 'function', not 'function_name')"));
        Error->SetStringField(TEXT("example"),
            TEXT("python_api_doc(class_name=\"IKRetargeterController\", function=\"reset_retarget_pose\")"));
        return Error;
    }
    if (!IsSafeIdentifier(ClassName) || !IsSafeIdentifier(FunctionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("invalid_identifier: '%s' / '%s' (pass a class name and a member name)"), *ClassName, *FunctionName));
    }

    const FString PyCode = MakeIntrospectionPrologue() + FString::Printf(TEXT(
        "name = \"%s\"\n"
        "member_name = \"%s\"\n"
        "cls = getattr(unreal, name, None)\n"
        "if cls is None:\n"
        "    _emit({'found': False, 'class_name': name, 'function': member_name, 'error': 'unknown_class',\n"
        "           'candidates': _candidates(unreal, name)})\n"
        "else:\n"
        "    member = getattr(cls, member_name, None)\n"
        "    if member is None:\n"
        "        _emit({'found': False, 'class_name': name, 'function': member_name, 'error': 'unknown_function',\n"
        "               'candidates': _candidates(cls, member_name)})\n"
        "    else:\n"
        "        doc = getattr(member, '__doc__', None) or ''\n"
        "        _emit({'found': True, 'class_name': name, 'function': member_name, 'module': _module(cls),\n"
        "               'signature': _sig(doc), 'doc': doc})\n"),
        *ClassName, *FunctionName);

    TSharedPtr<FJsonObject> Payload;
    TSharedPtr<FJsonObject> Error;
    if (!RunIntrospectionPython(PyCode, Payload, Error))
    {
        return Error;
    }

    bool bFound = false;
    Payload->TryGetBoolField(TEXT("found"), bFound);
    if (!bFound)
    {
        Payload->SetBoolField(TEXT("success"), false);
        if (!Payload->HasField(TEXT("error")))
        {
            Payload->SetStringField(TEXT("error"), TEXT("unknown_function"));
        }
        Payload->SetStringField(TEXT("hint"),
            TEXT("check the candidates list, or call python_api_index for the class"));
        return Payload;
    }

    Payload->SetBoolField(TEXT("success"), true);
    return Payload;
}

void FUnrealMCPPythonAPICommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "python_api_index", "python-api",
        "Read-only introspection of one engine python-exposed class (e.g. 'IKRetargeterController'): "
        "every documented member with its signature line and docstring, plus the owning module. "
        "Covers the engine python API layer that list_mcp_commands cannot see.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("class_name"), TEXT("string"), TEXT("Python class name, e.g. 'IKRigController'")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandlePythonAPIIndex(Params); });

    MCP_REGISTER_COMMAND(Registry, "python_api_doc", "python-api",
        "Read-only signature/doc lookup for one member of an engine python-exposed class. "
        "On a miss it returns the closest candidate names instead of raising a TypeError.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("class_name"), TEXT("string"), TEXT("Python class name, e.g. 'IKRigController'")),
            MCPParam(TEXT("function"), TEXT("string"), TEXT("Member name, e.g. 'add_retarget_chain'")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandlePythonAPIDoc(Params); });
}
