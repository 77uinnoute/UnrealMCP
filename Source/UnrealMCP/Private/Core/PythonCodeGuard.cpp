#include "Core/PythonCodeGuard.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

namespace
{
	/** Most findings reported per call; a runaway script should not produce an unbounded response. */
	constexpr int32 MaxMatches = 32;

	/**
	 * Blanks out comments and string literals, keeping every line (and every column) in place so the
	 * reported line numbers stay meaningful. Naming a banned API inside a comment or a generated
	 * string must not trip the guard.
	 */
	void StripNonCode(const FString& Source, TArray<FString>& OutLines)
	{
		enum class EState : uint8
		{
			Code,
			SingleQuote,
			DoubleQuote,
			TripleSingle,
			TripleDouble
		};

		OutLines.Reset();

		TArray<FString> RawLines;
		Source.ParseIntoArray(RawLines, TEXT("\n"), /*CullEmpty=*/false);

		EState State = EState::Code;

		for (const FString& RawLine : RawLines)
		{
			const int32 Length = RawLine.Len();
			FString Scrubbed;
			Scrubbed.Reserve(Length);

			for (int32 Index = 0; Index < Length; ++Index)
			{
				const TCHAR Char = RawLine[Index];
				const bool bHas = Index + 2 < Length;
				const bool bTripleSingle = bHas && Char == TEXT('\'') && RawLine[Index + 1] == TEXT('\'') && RawLine[Index + 2] == TEXT('\'');
				const bool bTripleDouble = bHas && Char == TEXT('"') && RawLine[Index + 1] == TEXT('"') && RawLine[Index + 2] == TEXT('"');

				switch (State)
				{
				case EState::Code:
					if (Char == TEXT('#'))
					{
						// Line comment: blank the remainder and stop scanning this line.
						Scrubbed.AppendChars(TEXT(" "), Length - Index);
						Index = Length;
					}
					else if (State == EState::Code && bTripleSingle)
					{
						Scrubbed.Append(TEXT("   "));
						Index += 2;
						State = EState::TripleSingle;
					}
					else if (State == EState::Code && bTripleDouble)
					{
						Scrubbed.Append(TEXT("   "));
						Index += 2;
						State = EState::TripleDouble;
					}
					else if (Char == TEXT('\''))
					{
						Scrubbed.Append(TEXT(" "));
						State = EState::SingleQuote;
					}
					else if (Char == TEXT('"'))
					{
						Scrubbed.Append(TEXT(" "));
						State = EState::DoubleQuote;
					}
					else
					{
						Scrubbed.AppendChar(Char);
					}
					break;

				case EState::SingleQuote:
				case EState::DoubleQuote:
				{
					const TCHAR Terminator = (State == EState::SingleQuote) ? TEXT('\'') : TEXT('"');
					if (Char == TEXT('\\') && Index + 1 < Length)
					{
						Scrubbed.Append(TEXT("  "));
						++Index;
					}
					else
					{
						Scrubbed.Append(TEXT(" "));
						if (Char == Terminator)
						{
							State = EState::Code;
						}
					}
					break;
				}

				case EState::TripleSingle:
				case EState::TripleDouble:
				{
					const TCHAR Terminator = (State == EState::TripleSingle) ? TEXT('\'') : TEXT('"');
					const bool bCloses = bHas
						&& Char == Terminator
						&& RawLine[Index + 1] == Terminator
						&& RawLine[Index + 2] == Terminator;

					if (bCloses)
					{
						Scrubbed.Append(TEXT("   "));
						Index += 2;
						State = EState::Code;
					}
					else
					{
						Scrubbed.Append(TEXT(" "));
					}
					break;
				}
				}
			}

			OutLines.Add(MoveTemp(Scrubbed));
		}
	}
}

namespace FPythonCodeGuard
{
	const TArray<FRule>& GetRules()
	{
		static const TArray<FRule> Rules = {
			// --- Wrong only while a PIE session owns the editor -------------------------------------
			// The EditorScriptingUtilities family funnels through UEditorAssetSubsystem, whose every
			// entry point starts with `if (!CheckIfInEditorAndPIE() || !EnsureAssetsLoaded()) return
			// <neutral>;` and that guard fails whenever GEditor->PlayWorld is set. The refusal comes
			// back as the same value a genuinely absent asset would produce.
			{
				TEXT("EditorAssetLibrary.does_asset_exist"),
				true,
				TEXT("while PIE runs this returns false for an asset that exists, so `if not exists: create_asset` walks into a duplicate-name modal dialog that freezes the whole bridge"),
				TEXT("asset_status (reports in_memory / on_disk / registry without answering false)")
			},
			{
				TEXT("EditorAssetLibrary.load_asset"),
				true,
				TEXT("while PIE runs this returns None for an asset that exists"),
				TEXT("unreal.load_asset (python-level LoadObject, no editor-scripting guard)")
			},
			{
				TEXT("EditorAssetLibrary.find_asset_data"),
				true,
				TEXT("while PIE runs this returns an empty FAssetData for an asset that exists"),
				TEXT("asset_status")
			},
			{
				TEXT("EditorAssetLibrary.list_assets"),
				true,
				TEXT("while PIE runs this returns an empty list"),
				TEXT("asset_status, or unreal.EditorAssetLibrary.list_assets after stop_pie")
			},
			{
				TEXT("EditorAssetLibrary.load_blueprint_class"),
				true,
				TEXT("same guard as the rest of the family: None while PIE runs"),
				TEXT("unreal.load_object / unreal.load_class on the generated class")
			},

			// --- Wrong whenever they are called ------------------------------------------------------
			{
				TEXT("EditorAssetLibrary.delete_asset"),
				false,
				TEXT("deleting an asset that is still referenced or freshly created can mark its package potentially corrupt, and the editor AVs a couple of seconds later"),
				TEXT("safe_delete_asset (reports blockers; force=true breaks the ones it can, and refuses when it cannot)")
			},
			{
				TEXT(".create_asset("),
				false,
				TEXT("a name clash pops a modal duplicate-name dialog, which occupies the game thread and times out every bridge command including read-only ones"),
				TEXT("create_asset_safe (returns asset_exists instead of a dialog)")
			},
			{
				TEXT(".duplicate_asset("),
				false,
				TEXT("same modal-on-clash behaviour as create_asset"),
				TEXT("duplicate_asset_safe")
			},
			{
				TEXT("AssetImportTask("),
				false,
				TEXT("a hand-rolled import task lets Interchange drain the game-thread task queue, which kills the editor process outright rather than returning an error"),
				TEXT("import_assets")
			},
			{
				TEXT("import_asset_tasks("),
				false,
				TEXT("same as a hand-rolled AssetImportTask: the editor process dies instead of failing"),
				TEXT("import_assets")
			},

			// --- WARNING only: reported, but the call still runs ------------------------------------
			// Every engine "save an asset" entry point funnels into
			// UEditorLoadingAndSavingUtils::SavePackages, which runs
			// FEditorFileUtils::InternalPromptForCheckoutAndSave and can pop a modal when it decides a
			// checkout is needed - a modal owns the game thread, so every bridge command (including
			// read-only ones) times out until a human closes it. The plugin's own persist path sidesteps
			// that (UPackage::SavePackage, see SaveAssetForObject), so a script that saves by hand is
			// stepping around the guard the plugin just built for itself.
			//
			// These are warnings rather than refusals on purpose. Measured: ~150 scratch-script call sites
			// save assets this way, so refusing would break working scripts. A warning is earned here
			// because a replacement exists: whatever command made the change can persist it
			// (persist=true / save=true), and that path is dialog-free.
			//
			// Deliberately NOT listed: EditorLoadingAndSavingUtils.save_packages / save_dirty_packages.
			// They sit in the same engine save family, but nothing replaces them - save_dirty_packages is
			// the only flush that reaches World Partition external-actor packages, and the plugin's own
			// skills recommend it. Warning about a call the caller has no alternative to is noise, and it
			// would push people off the documented path.
			{
				TEXT("EditorAssetLibrary.save_asset("),
				false,
				TEXT("goes through the engine save path (SavePackages -> InternalPromptForCheckoutAndSave), which can prompt for a checkout and freeze the whole bridge"),
				TEXT("persist the change where it was made: recompile_material(save=true) / set_object_property(persist=true) / create_asset_safe(persist=true)"),
				false
			},
			{
				TEXT("EditorAssetLibrary.save_loaded_asset("),
				false,
				TEXT("same engine save path as save_asset"),
				TEXT("a plugin command with persist=true"),
				false
			},
			{
				TEXT("EditorAssetLibrary.save_loaded_assets("),
				false,
				TEXT("same engine save path as save_asset"),
				TEXT("a plugin command with persist=true"),
				false
			},
			{
				TEXT("EditorAssetLibrary.save_directory("),
				false,
				TEXT("same engine save path, and it writes every asset in the directory whether or not this script touched it"),
				TEXT("persist only what this script changed, through the command that changed it"),
				false
			},
		};
		return Rules;
	}

	const TArray<FLoopbackRule>& GetLoopbackRules()
	{
		static const TArray<FLoopbackRule> Rules = {
			// Commands whose implementation forces a garbage collection. Collecting while python is
			// executing hits UObjectBaseUtility::MarkAsGarbage()'s check(!IsRooted()) and kills the
			// editor process outright (2026-10-12: wipe_material_graph over the loopback; assert at
			// UObjectBaseUtility.h:179, then AV, no structured error). The plugin now skips those
			// collections while python runs (Core/MCPGCSafety.h), which turns the crash into a silent
			// no-op - so the loopback call is refused here instead, and the caller is told to use the
			// tool face where the reclaim actually happens.
			{
				TEXT("wipe_material_graph"),
				TEXT("its delete loop calls CollectGarbage; forcing a collection while python executes crashes the editor (measured 2026-10-12)"),
				TEXT("call wipe_material_graph from the MCP tool face, or delete nodes with delete_material_expressions from python"),
				true
			},
			{
				TEXT("safe_delete_asset"),
				TEXT("its delete path calls CollectGarbage, and that collection is skipped while python runs, so the name is NOT reclaimed and a later create reports asset_exists"),
				TEXT("safe_delete_asset from the MCP tool face (the surrounding script keeps working; only this call moves out)"),
				false
			},
			{
				TEXT("delete_actor"),
				TEXT("with flush=true it calls CollectGarbage, which is skipped while python runs, so the actor name is NOT released for a same-name respawn in this script"),
				TEXT("delete_actor(flush=true) from the MCP tool face, or respawn in a later call"),
				false
			},
		};
		return Rules;
	}

	bool IsPlaying()
	{
#if WITH_EDITOR
		// Matches EditorScriptingUtils::IsInEditorAndNotPlaying, which is what makes the family above
		// refuse to answer.
		return GEditor != nullptr && GEditor->PlayWorld != nullptr;
#else
		return false;
#endif
	}

	bool Check(const FString& Source, TArray<FPythonGuardMatch>& OutMatches)
	{
		OutMatches.Reset();

		if (Source.IsEmpty())
		{
			return true;
		}

		TArray<FString> CodeLines;
		StripNonCode(Source, CodeLines);

		const bool bPlaying = IsPlaying();
		bool bBlocked = false;
		bool bCapped = false;

		for (const FRule& Rule : GetRules())
		{
			if (Rule.bOnlyWhilePlaying && !bPlaying)
			{
				continue;
			}

			for (int32 LineIndex = 0; LineIndex < CodeLines.Num(); ++LineIndex)
			{
				if (!CodeLines[LineIndex].Contains(Rule.Pattern))
				{
					continue;
				}

				FPythonGuardMatch Match;
				Match.Line = LineIndex + 1;
				Match.Api = Rule.Pattern;
				Match.Reason = Rule.Reason;
				Match.UseInstead = Rule.UseInstead;
				Match.bBlocks = Rule.bBlocks;
				OutMatches.Add(MoveTemp(Match));

				if (Rule.bBlocks)
				{
					bBlocked = true;
				}

				if (OutMatches.Num() >= MaxMatches)
				{
					// Stop scanning, but keep the verdict: what is already in OutMatches decides whether
					// the source runs, and a job that only tripped warnings must still execute.
					bCapped = true;
					break;
				}
			}

			if (bCapped)
			{
				break;
			}
		}

		// Loopback-only rules: scanned against the RAW source, because a command name can only appear
		// inside a string literal (which StripNonCode blanks). Requiring the loopback entry point on
		// the same line is what keeps a plain mention of the name - a comment, a docstring, a log
		// message - from tripping the guard.
		TArray<FString> RawLines;
		Source.ParseIntoArray(RawLines, TEXT("\n"), /*CullEmpty=*/false);

		for (const FLoopbackRule& Rule : GetLoopbackRules())
		{
			for (int32 LineIndex = 0; LineIndex < RawLines.Num(); ++LineIndex)
			{
				const FString& Line = RawLines[LineIndex];
				const bool bLoopbackCall = Line.Contains(TEXT("execute_mcp_command")) || Line.Contains(TEXT("send_command"));
				if (!bLoopbackCall || !Line.Contains(Rule.Command))
				{
					continue;
				}

				FPythonGuardMatch Match;
				Match.Line = LineIndex + 1;
				Match.Api = FString::Printf(TEXT("execute_mcp_command(\"%s\")"), Rule.Command);
				Match.Reason = Rule.Reason;
				Match.UseInstead = Rule.UseInstead;
				Match.bBlocks = Rule.bBlocks;
				OutMatches.Add(MoveTemp(Match));

				if (Rule.bBlocks)
				{
					bBlocked = true;
				}

				if (OutMatches.Num() >= MaxMatches)
				{
					return !bBlocked;
				}
			}
		}

		return !bBlocked;
	}
}
