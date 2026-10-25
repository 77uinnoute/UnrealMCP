#pragma once

#include "CoreMinimal.h"

/**
 * Static guard for python handed to the bridge.
 *
 * Every path that runs python inside the editor funnels through ExecutePythonCode() - both the
 * execute_python_command / execute_python_file tools and every plugin tool that goes through
 * ue_safe_api. The source is base64-wrapped just before exec, so this is the only layer that can
 * still see the plain text.
 *
 * The rules below come from measured failures, not from taste:
 *   - the EditorScriptingUtilities family refuses to answer while PIE runs and encodes that refusal
 *     as the same value a genuine "missing" would produce, so `if not exists: create` walks into a
 *     duplicate-name modal dialog that freezes the whole bridge;
 *   - EditorAssetLibrary.delete_asset can mark a package potentially-corrupt and AV seconds later;
 *   - AssetTools create_asset / duplicate_asset pop a modal on a name clash;
 *   - a hand-rolled AssetImportTask + import_asset_tasks run kills the editor process outright.
 */
struct FPythonGuardMatch
{
	/** 1-based line in the submitted source. */
	int32 Line = 0;

	FString Api;

	FString Reason;

	FString UseInstead;

	/**
	 * True: the call is refused and nothing runs. False: the source still runs and the match comes
	 * back as a warning, because the API is a documented part of some workflow and banning it would
	 * break working scripts (measured: ~150 scratch scripts save assets and flush packages this way).
	 */
	bool bBlocks = true;
};

namespace FPythonCodeGuard
{
	struct FRule
	{
		/** Substring that identifies the call. Chosen so that the sanctioned alternative cannot match. */
		const TCHAR* Pattern;

		/** True when the API is only wrong while a PIE session is running. */
		bool bOnlyWhilePlaying;

		const TCHAR* Reason;

		const TCHAR* UseInstead;

		/** True to refuse the call, false to merely report it (see FPythonGuardMatch::bBlocks). */
		bool bBlocks = true;
	};

	/** Rule table, in reporting order. */
	const TArray<FRule>& GetRules();

	/**
	 * Commands that must not be reached over the editor python loopback.
	 *
	 * The table above scans scrubbed source, so it can never see a command name (that name lives in a
	 * string literal). These rules are matched against the RAW line and only when that same line also
	 * calls execute_mcp_command / send_command, which is what keeps an honest mention of a command
	 * name (a comment, a docstring, a log message) from tripping the guard.
	 */
	struct FLoopbackRule
	{
		/** Exact bridge command name as written in the python literal. */
		const TCHAR* Command;

		const TCHAR* Reason;

		const TCHAR* UseInstead;

		/** True to refuse, false to warn (see FPythonGuardMatch::bBlocks). */
		bool bBlocks = true;
	};

	const TArray<FLoopbackRule>& GetLoopbackRules();

	/** True while a PIE/simulate session owns the editor. */
	bool IsPlaying();

	/**
	 * Lexer-aware scan: comments and string literals are skipped, so merely naming a banned API in
	 * documentation or in a generated string does not trip the guard.
	 *
	 * @return true when the source may run - i.e. when OutMatches holds no BLOCKING finding.
	 *         OutMatches can still be non-empty in that case: those entries are warnings the caller
	 *         is expected to attach to the successful response.
	 */
	bool Check(const FString& Source, TArray<FPythonGuardMatch>& OutMatches);
}
