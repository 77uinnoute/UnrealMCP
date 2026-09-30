$ErrorActionPreference = 'Stop'

$root = 'e:\ue_proj\asset_test_UE55 (2)\asset_test_UE55\Plugins\UnrealMCP\Content\Python\scripts\'
$files = @(
    'node\test_component_reference.py',
    'blueprints\test_create_and_spawn_blueprints_with_different_components.py',
    'node\test_create_bird_blueprint_with_input_and_camera.py'
)

$importBlock = @'
import json
from typing import Dict, Any, Optional, List

import unreal

# NOTE: this script runs inside the editor through the MCP tool `execute_python_file`, so it must not
# reach out over TCP or rely on __file__ (the file is exec'd, not imported).
# The editor's python has already configured `logging`, so records would land in the engine log as
# Errors and the bridge would report the job as failed - log to stdout instead.
class _PrintLogger:
    def info(self, message):
        print("INFO: " + str(message))

    def warning(self, message):
        print("WARN: " + str(message))

    def error(self, message):
        print("FAIL: " + str(message))


logger = _PrintLogger()
'@

$sendCommand = @'
def send_command(command: str, params: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """Run one bridge command in-process (the sanctioned in-editor route).

    The response envelope is the same one the TCP path returns
    ({"status": "success", "result": ...}), which is what the callers below read.
    """
    try:
        raw = unreal.UnrealMCPPythonAPI.execute_mcp_command(command, json.dumps(params))
        response = json.loads(raw)
        logger.info("Executed command: " + command)
        return response
    except Exception as e:
        logger.error("Error sending command: " + str(e))
        return None

'@

$report = @()

foreach ($relative in $files) {
    $path = $root + $relative
    $t = [IO.File]::ReadAllText($path)
    $before = $t.Length

    # 1. imports + logging block -> stdout logger
    $i = $t.IndexOf("import sys")
    $j = $t.IndexOf("logger = logging.getLogger(")
    if ($i -lt 0 -or $j -le $i) { $report += "MISS imports $relative"; continue }
    $j = $t.IndexOf("`n", $j) + 1
    $t = $t.Remove($i, $j - $i).Insert($i, $importBlock)

    # 2. the whole socket send_command -> in-editor loopback
    $i = $t.IndexOf("def send_command(")
    $nextDef = $t.IndexOf("`ndef ", $i + 10)
    if ($i -lt 0 -or $nextDef -le $i) { $report += "MISS send_command $relative"; continue }
    $t = $t.Remove($i, $nextDef - $i).Insert($i, $sendCommand)

    # 3. per-call socket creation (both indentations)
    foreach ($indent in @('        ', '            ')) {
        $open = $indent + "sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)`r`n" + $indent + "sock.connect((`"127.0.0.1`", 55557))`r`n"
        $t = $t.Replace($open, "")
        $openLf = $indent + "sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)`n" + $indent + "sock.connect((`"127.0.0.1`", 55557))`n"
        $t = $t.Replace($openLf, "")
        $close = $indent + "sock.close()`r`n"
        $t = $t.Replace($close, "")
        $t = $t.Replace($indent + "sock.close()`n", "")
    }

    # 4. call sites that passed the socket in
    $t = $t.Replace("send_command(sock, ", "send_command(")

    # 5. the file is exec'd, so __name__ is never "__main__"
    $t = $t.Replace('if __name__ == "__main__":', 'if True:  # exec''d by the MCP runner, so __name__ is not "__main__"')

    [IO.File]::WriteAllText($path, $t)
    $report += ("ok {0} {1} -> {2} chars" -f $relative, $before, $t.Length)
}

$report -join "`n"
