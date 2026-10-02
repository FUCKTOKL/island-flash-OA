#pragma once
// 预编译头：WinUI3 + C++/WinRT 常用头统一在此（模板惯例，改这里全量重编）
#include <windows.h>
#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>

// WinRT 基础
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
// 本项目生成的运行时类（App / IslandWindow）
#include <winrt/IslandApp.h>
// WinUI3（Microsoft.UI.*）
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
// 窗口管理（AppWindow / OverlappedPresenter / DisplayArea）
#include <winrt/Microsoft.UI.Windowing.h>
// XAML-Composition 互操作（ElementCompositionPreview）
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Composition.h>
// 时钟/定时器/输入
#include <winrt/Windows.System.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Globalization.DateTimeFormatting.h> // DateTimeFormatter 在此命名空间
// FontWeights 在 Microsoft.UI.Text 命名空间
#include <winrt/Microsoft.UI.Text.h>
// RectInt32（窗口几何）
#include <winrt/Windows.Graphics.h>
