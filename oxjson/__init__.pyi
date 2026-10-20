import json
from collections.abc import Callable
from typing import Any, Final

JSON_ERROR_EMPTY_DOCUMENT: Final[int]

class JSONError(ValueError):
    code: int
    pos: int

class JSONDecodeError(json.JSONDecodeError, JSONError):
    code: int

def loads(data: str | bytes | bytearray | memoryview) -> Any: ...
def load(fp: Any) -> Any: ...
OPT_APPEND_NEWLINE: Final[int]
OPT_INDENT_2: Final[int]
OPT_NAIVE_UTC: Final[int]
OPT_NON_STR_KEYS: Final[int]
OPT_OMIT_MICROSECONDS: Final[int]
OPT_PASSTHROUGH_DATACLASS: Final[int]
OPT_PASSTHROUGH_DATETIME: Final[int]
OPT_PASSTHROUGH_SUBCLASS: Final[int]
OPT_SERIALIZE_DATACLASS: Final[int]
OPT_SERIALIZE_NUMPY: Final[int]
OPT_SERIALIZE_UUID: Final[int]
OPT_SORT_KEYS: Final[int]
OPT_STRICT_INTEGER: Final[int]
OPT_UTC_Z: Final[int]

def dumps(
    obj: Any,
    default: Callable[[Any], Any] | None = None,
    option: int | None = None,
    *,
    allow_nan: bool = True,
) -> bytes: ...
def dump(obj: Any, fp: Any, **kwargs: Any) -> None: ...
