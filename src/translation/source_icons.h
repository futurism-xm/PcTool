#pragma once
#include <windows.h>
#include "translation/source_config.h"
namespace translation { void DrawSourceIcon(HDC dc,SourceKind kind,const RECT& rect); }
namespace translation { void DrawVisibilityIcon(HDC dc,bool visible,const RECT& rect,COLORREF color); }
