# Type stub for the embedded `dolphin.debug` module.
# Manage PowerPC code/memory breakpoints; hits surface via the event module.

from typing import Any, Dict

def set_breakpoint(addr: int) -> None: ...
def remove_breakpoint(addr: int) -> None: ...

# Memory breakpoint config dict: either "At" (single addr) or "Start"+"End" (range);
# optional "BreakOnRead" (default True), "BreakOnWrite" (default False),
# "LogOnHit" (default True), "BreakOnHit" (default True), "Condition" (expr str).
def set_memory_breakpoint(config: Dict[str, Any]) -> None: ...
def remove_memory_breakpoint(addr: int) -> None: ...

# Non-pausing memory observer. Config accepts either "At" or "Start"+"End",
# optional "WatchOnRead" (default True), "WatchOnWrite" (default False), and
# "Condition" (expr str). Hits are delivered by event.on_memorywatch.
# Watches are automatically removed when their script unloads.
def set_memory_watch(config: Dict[str, Any]) -> None: ...
