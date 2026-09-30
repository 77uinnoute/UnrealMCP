#include "Commands/Material/UnrealMCPMaterialHlslLint.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"

namespace
{
    // 'SceneTexturesStruct' is the fatal one: the pass binds
    // FOpaqueBasePassUniformParameters at the SceneTextures slot, so a shader
    // expecting the named struct trips the RHI assert at draw time. Every
    // '*UniformParameters' name has the same shape.
    const TCHAR* const UBStructFix =
        TEXT("Nearest-hit fix: fetch through a scene texture input pin instead - ")
        TEXT("pin.Fetch(pixelOffset), SceneTextureLookup(id, uv), SceneTextureFetchFunc(...), ")
        TEXT("or CalcSceneCustomDepth(uv). Naming the uniform buffer crashes the editor ")
        TEXT("(RHICoreShader.cpp:55 assertion), it is not a compile error.");

    const TCHAR* const IncludeFix = TEXT("Use a virtual path such as \"/Engine/Private/...\".");
    const TCHAR* const FetchFix = TEXT("Use pin.Fetch(pixelOffset) or CalcSceneCustomDepth(uv) and let the engine convert.");

    bool RegexContains(const FRegexPattern& Pattern, const FString& Text)
    {
        FRegexMatcher Matcher(Pattern, Text);
        return Matcher.FindNext();
    }

    template <typename FuncType>
    void ForEachMatch(const FRegexPattern& Pattern, const FString& Text, FuncType&& Func)
    {
        FRegexMatcher Matcher(Pattern, Text);
        while (Matcher.FindNext())
        {
            Func(Matcher);
        }
    }

    // Mirror of python str.split()[-1]: the text after the last whitespace.
    FString LastWhitespaceToken(const FString& Text)
    {
        for (int32 Index = Text.Len() - 1; Index >= 0; --Index)
        {
            if (FChar::IsWhitespace(Text[Index]))
            {
                return Text.Mid(Index + 1);
            }
        }
        return Text;
    }

    int32 LineNumberOfOffset(const FString& Text, int32 Offset)
    {
        const int32 Limit = FMath::Min(Offset, Text.Len());
        int32 LineCount = 1;
        for (int32 Index = 0; Index < Limit; ++Index)
        {
            if (Text[Index] == TEXT('\n'))
            {
                ++LineCount;
            }
        }
        return LineCount;
    }

    TArray<FString> StripComments(const TArray<FString>& RawLines)
    {
        TArray<FString> Stripped;
        Stripped.Reserve(RawLines.Num());
        bool bInBlock = false;
        for (const FString& Line : RawLines)
        {
            FString Kept;
            int32 Index = 0;
            const int32 Length = Line.Len();
            while (Index < Length)
            {
                if (bInBlock)
                {
                    const int32 End = Line.Find(TEXT("*/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index);
                    if (End == INDEX_NONE)
                    {
                        break;
                    }
                    bInBlock = false;
                    Index = End + 2;
                    continue;
                }
                if (Line.Mid(Index, 2) == TEXT("//"))
                {
                    break;
                }
                if (Line.Mid(Index, 2) == TEXT("/*"))
                {
                    bInBlock = true;
                    Index += 2;
                    continue;
                }
                Kept.AppendChar(Line[Index]);
                ++Index;
            }
            Stripped.Add(Kept);
        }
        return Stripped;
    }

    struct FLintReport
    {
        const TArray<FString>* RawLines = nullptr;
        TArray<TSharedPtr<FJsonValue>> Errors;
        TArray<TSharedPtr<FJsonValue>> Warnings;

        void Report(TArray<TSharedPtr<FJsonValue>>& Bucket, int32 LineNo, const TCHAR* Rule, const FString& Message, const FString& Suggestion)
        {
            FString Snippet;
            if (RawLines && LineNo > 0 && LineNo <= RawLines->Num())
            {
                Snippet = (*RawLines)[LineNo - 1].TrimStartAndEnd();
            }
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetNumberField(TEXT("line"), LineNo);
            Item->SetStringField(TEXT("rule"), Rule);
            Item->SetStringField(TEXT("message"), Message);
            Item->SetStringField(TEXT("suggestion"), Suggestion);
            Item->SetStringField(TEXT("snippet"), Snippet.Left(200));
            Bucket.Add(MakeShared<FJsonValueObject>(Item));
        }

        void Error(int32 LineNo, const TCHAR* Rule, const FString& Message, const FString& Suggestion)
        {
            Report(Errors, LineNo, Rule, Message, Suggestion);
        }

        void Warning(int32 LineNo, const TCHAR* Rule, const FString& Message, const FString& Suggestion)
        {
            Report(Warnings, LineNo, Rule, Message, Suggestion);
        }
    };

    const TMap<FString, int32>& OutputTypeComponentCounts()
    {
        static const TMap<FString, int32> Counts = {
            { TEXT("Float1"), 1 },
            { TEXT("Float2"), 2 },
            { TEXT("Float3"), 3 },
            { TEXT("Float4"), 4 },
        };
        return Counts;
    }
}

TArray<FString> FUnrealMCPMaterialHlslLint::StripHlslComments(const FString& Code)
{
    TArray<FString> RawLines;
    Code.ParseIntoArrayLines(RawLines);
    return StripComments(RawLines);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialHlslLint::Run(const FString& Code, const FString& OutputType)
{
    TArray<FString> RawLines;
    Code.ParseIntoArrayLines(RawLines);
    const TArray<FString> Stripped = StripComments(RawLines);
    const FString Joined = FString::Join(Stripped, TEXT("\n"));

    FLintReport Report;
    Report.RawLines = &RawLines;

    // ---- per-line hazards ----
    static const FRegexPattern UBNamePatterns[] = {
        FRegexPattern(TEXT("\\bSceneTexturesStruct\\b")),
        FRegexPattern(TEXT("\\bMobileSceneTextures\\b")),
        FRegexPattern(TEXT("\\bSingleLayerWater\\b")),
    };
    static const TCHAR* const UBNameLiterals[] = {
        TEXT("SceneTexturesStruct"),
        TEXT("MobileSceneTextures"),
        TEXT("SingleLayerWater"),
    };
    static const FRegexPattern UBStructPattern(TEXT("\\b(\\w*UniformParameters)\\b"));
    static const FRegexPattern ViewMemberPattern(TEXT("\\bView\\s*\\.\\s*(\\w+)"));
    static const FRegexPattern ResolvedViewPattern(TEXT("\\b(?:ResolvedView|PrimaryView)\\s*\\.\\s*(\\w+)"));
    static const FRegexPattern IncludePattern(TEXT("#\\s*include\\s*\"([^\"]*)\""));
    static const FRegexPattern ManualUVPattern(TEXT("\\bViewportUVToBufferUV\\s*\\("));
    static const FRegexPattern CalcSceneDepthPattern(TEXT("\\bCalcSceneDepth\\s*\\(([^)]*)\\)"));

    for (int32 LineIdx = 0; LineIdx < Stripped.Num(); ++LineIdx)
    {
        const int32 LineNo = LineIdx + 1;
        const FString& Line = Stripped[LineIdx];

        for (int32 NameIdx = 0; NameIdx < UE_ARRAY_COUNT(UBNamePatterns); ++NameIdx)
        {
            if (RegexContains(UBNamePatterns[NameIdx], Line))
            {
                Report.Error(LineNo, TEXT("shader_parameter_struct"),
                    FString::Printf(TEXT("'%s' is a shader parameter struct name."), UBNameLiterals[NameIdx]), UBStructFix);
            }
        }
        ForEachMatch(UBStructPattern, Line, [&Report, LineNo](FRegexMatcher& Matcher)
        {
            Report.Error(LineNo, TEXT("shader_parameter_struct"),
                FString::Printf(TEXT("'%s' is a shader parameter struct name."), *Matcher.GetCaptureGroup(1)), UBStructFix);
        });
        ForEachMatch(ViewMemberPattern, Line, [&Report, LineNo](FRegexMatcher& Matcher)
        {
            const FString Member = Matcher.GetCaptureGroup(1);
            if (Member == TEXT("GameTime"))
            {
                Report.Error(LineNo, TEXT("view_gametime"),
                    TEXT("View.GameTime is frozen in non-PIE editor viewports, so the effect silently will not animate."),
                    TEXT("Use View.RealTime."));
            }
            else if (Member != TEXT("RealTime") && Member != TEXT("BufferSizeAndInvSize") && Member != TEXT("ViewSizeAndInvSize"))
            {
                Report.Warning(LineNo, TEXT("view_member"),
                    FString::Printf(TEXT("'View.%s' is not a View member the material translator emits; Custom nodes have reported \"use of undeclared identifier 'View'\"."), *Member),
                    TEXT("Use an engine helper instead: GetTanHalfFieldOfView(), GetSceneTextureViewSize(id), SvPositionToWorld(), SvPositionToTranslatedWorld(), or a SceneTexture node's Coordinates/Size output."));
            }
        });
        ForEachMatch(ResolvedViewPattern, Line, [&Report, LineNo](FRegexMatcher& Matcher)
        {
            Report.Warning(LineNo, TEXT("resolved_view_member"),
                FString::Printf(TEXT("'%s' is not guaranteed inside a Custom node."), *Matcher.GetCaptureGroup(0)),
                TEXT("Use the engine helper functions (GetPreViewTranslation / GetWorldViewOrigin / SvPositionToWorld ...) or a node's output instead."));
        });
        ForEachMatch(IncludePattern, Line, [&Report, LineNo](FRegexMatcher& Matcher)
        {
            const FString IncludePath = Matcher.GetCaptureGroup(1);
            if (!IncludePath.StartsWith(TEXT("/")))
            {
                Report.Error(LineNo, TEXT("include_not_virtual"),
                    FString::Printf(TEXT("#include \"%s\" is not a virtual shader path."), *IncludePath), IncludeFix);
            }
        });
        if (RegexContains(ManualUVPattern, Line))
        {
            Report.Warning(LineNo, TEXT("manual_buffer_uv"),
                TEXT("Hand-written buffer UV conversion easily lands in the wrong space (outline only in one screen corner, or an all-white screen)."),
                TEXT("Use pin.Fetch(pixelOffset) on the scene texture input, or ViewportUVToSceneTextureUV(uv, id)."));
        }
        ForEachMatch(CalcSceneDepthPattern, Line, [&Report, LineNo](FRegexMatcher& Matcher)
        {
            if (Matcher.GetCaptureGroup(1).Contains(TEXT("*")))
            {
                Report.Warning(LineNo, TEXT("manual_uv_size_math"),
                    TEXT("CalcSceneDepth with hand-multiplied UV/size mixes viewport UV with buffer pixel space."), FetchFix);
            }
        });
    }

    // ---- Input: declarations ----
    static const FRegexPattern InputLinePattern(TEXT("^\\s*Input\\s*:\\s*(.*)$"), ERegexPatternFlags::CaseInsensitive);
    static const FRegexPattern InputNamePattern(TEXT("^([A-Za-z_]\\w*)"));

    TArray<TPair<FString, int32>> DeclaredInputs;
    TMap<FString, int32> DeclaredInputLines;
    for (int32 LineIdx = 0; LineIdx < Stripped.Num(); ++LineIdx)
    {
        const int32 LineNo = LineIdx + 1;
        FString Declaration;
        {
            FRegexMatcher Matcher(InputLinePattern, Stripped[LineIdx]);
            if (!Matcher.FindNext())
            {
                continue;
            }
            Declaration = Matcher.GetCaptureGroup(1).TrimStartAndEnd();
        }

        FString Name;
        {
            FRegexMatcher Matcher(InputNamePattern, Declaration);
            if (!Matcher.FindNext())
            {
                Report.Error(LineNo, TEXT("input_decl_syntax"),
                    FString::Printf(TEXT("Input declaration has no usable name: '%s'."), *Declaration),
                    TEXT("Declare it as 'Input: Name' or 'Input: Name(Type)' / 'Input: Name(Texture2D, SamplerType)'."));
                continue;
            }
            Name = Matcher.GetCaptureGroup(1);
        }

        if (const int32* FirstLine = DeclaredInputLines.Find(Name))
        {
            Report.Error(LineNo, TEXT("input_decl_duplicate"),
                FString::Printf(TEXT("Input '%s' is declared twice (first at line %d)."), *Name, *FirstLine),
                TEXT("Give each input a unique name."));
        }
        else
        {
            DeclaredInputLines.Add(Name, LineNo);
            DeclaredInputs.Add(TPair<FString, int32>(Name, LineNo));
        }
    }

    for (const TPair<FString, int32>& Declared : DeclaredInputs)
    {
        const FString ReferencePatternSource = FString::Printf(TEXT("\\b%s(?:Sampler)?\\b"), *Declared.Key);
        const FRegexPattern ReferencePattern(ReferencePatternSource);

        bool bUsed = false;
        for (int32 LineIdx = 0; LineIdx < Stripped.Num(); ++LineIdx)
        {
            if (LineIdx + 1 == Declared.Value)
            {
                continue;
            }
            FRegexMatcher InputLineMatcher(InputLinePattern, Stripped[LineIdx]);
            if (InputLineMatcher.FindNext())
            {
                continue;
            }
            if (RegexContains(ReferencePattern, Stripped[LineIdx]))
            {
                bUsed = true;
                break;
            }
        }
        if (!bUsed)
        {
            Report.Warning(Declared.Value, TEXT("input_unused"),
                FString::Printf(TEXT("Declared input '%s' is never referenced in the body; its pin can be left unconnected but is dead weight."), *Declared.Key),
                TEXT("Remove the declaration, or reference it (the engine also passes it as '<name>Sampler' for Texture2D inputs)."));
        }
    }

    // ---- return statement ----
    static const FRegexPattern ReturnPattern(TEXT("\\breturn\\b"));
    if (!RegexContains(ReturnPattern, Joined))
    {
        Report.Error(1, TEXT("missing_return"),
            TEXT("The code has no 'return' statement; a Custom node must return its output."),
            TEXT("Return the value for the node's Output Type (e.g. 'return color;')."));
    }

    if (!OutputType.IsEmpty())
    {
        FString LastPart = OutputType;
        int32 UnderscoreIdx = INDEX_NONE;
        if (OutputType.FindLastChar(TEXT('_'), UnderscoreIdx))
        {
            LastPart = OutputType.Mid(UnderscoreIdx + 1);
        }
        LastPart.TrimStartAndEndInline();

        if (const int32* Wanted = OutputTypeComponentCounts().Find(LastPart))
        {
            static const FRegexPattern ReturnCtorPattern(TEXT("\\breturn\\s+(?:half|float|int|uint|bool)([1-4])\\s*\\("));
            ForEachMatch(ReturnCtorPattern, Joined, [&Report, &Joined, &OutputType, Wanted](FRegexMatcher& Matcher)
            {
                const FString Components = Matcher.GetCaptureGroup(1);
                if (FCString::Atoi(*Components) == *Wanted)
                {
                    return;
                }
                Report.Warning(LineNumberOfOffset(Joined, Matcher.GetMatchBeginning()), TEXT("output_type_component_count"),
                    FString::Printf(TEXT("'return %s' returns %s component(s) but Output Type %s expects %d."),
                        *LastWhitespaceToken(Matcher.GetCaptureGroup(0)), *Components, *OutputType, *Wanted),
                    TEXT("Match the value to the node's Output Type, or change the Output Type."));
            });
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetBoolField(TEXT("ok"), Report.Errors.Num() == 0);
    Result->SetNumberField(TEXT("error_count"), Report.Errors.Num());
    Result->SetNumberField(TEXT("warning_count"), Report.Warnings.Num());
    Result->SetArrayField(TEXT("errors"), Report.Errors);
    Result->SetArrayField(TEXT("warnings"), Report.Warnings);
    return Result;
}
