# 简体中文本地化（zh-CN）

> 界面预览与功能概览见 [readme.zh-CN.md](../readme.zh-CN.md)；本文记录汉化范围、字体和实现细节。

SDRIAK 的界面在此分支已汉化为简体中文：核心界面、信号源/输出设备面板、解码器面板以及
频率管理器、录音、扫描器、定时任务等杂项模块的用户可见文本均为中文。

## 覆盖范围

- 核心界面：主窗口、菜单、对话框、瀑布图/频谱悬浮信息、频率输入与频段选择器、模块管理器。
- 模块面板：`source_modules/`、`sink_modules/`、`decoder_modules/`、`misc_modules/` 中
  ImGui / SmGui 渲染的标签、按钮、表头、下拉选项、提示与状态文本。
- 加载界面与退出确认等系统对话框。

**有意保留英文**的内容：

- 技术标识与配置值：配置键、模块实例名、sink provider 名、菜单顺序键、JSON 键。
  这样才能保证升级到中文版后旧配置继续可用。
- 单位与缩写：`Hz/kHz/MHz/dB/dBm/dBFS/ppm/fps/ms`、`AM/FM/NFM/WFM/SSB/CW/RDS/CTCSS/DCS`、
  `M17/APRS/DAB/SSTV/NOAA` 等。
- 硬件、厂商与卫星/呼号专名：`Airspy`、`HackRF`、`RTL-SDR`、`LimeSDR`、`USRP` 等。
- 频段名称表（`core/src/gui/widgets/band_mapping.cpp`）与 `root/res/bandplans/*.json`：
  这些是监管/通用频段名称（如 `AM Broadcast`、`2200m`），同时也是频段分类与稳定 ID 的
  依据，保留英文可避免误分类。
- 日志、异常与命令行帮助：不属于图形界面。
- 模块 `SDRPP_MOD_INFO` 的 `description` 字段：本分支的模块管理器不显示该字段。

## 实现方式

1. **源码内 UTF-8 中文字面量**：界面文本直接写在源码里；MSVC 需要 `/utf-8` 才能正确
   解释这些字面量，见根 `CMakeLists.txt`。
2. **中文字体**：`root/res/fonts/NotoSansSC-Regular-subset.ttf` 是 Noto Sans SC 的静态子集
   （OFL 许可，见 `root/res/licenses/NotoSansSC-OFL.txt`）。`core/src/gui/style.cpp` 在加载
   Roboto 后以 `MergeMode` 把它并入基础字体；桌面端字形范围额外包含 ImGui 常用的 2500 个
   汉字（书签等用户输入也能显示），Android 端只含界面实际用字以控制显存。
3. **字形范围表**：`core/src/gui/cjk_glyphs.h` 由脚本从源码中实际出现的汉字生成，因此字体
   图集只为真正用到的字符付出开销。
4. **菜单标题映射**：模块实例名是配置键与菜单顺序键，不能直接翻译。`core/src/gui/widgets/menu_names.h`
   提供 `menuDisplayName()`，只在绘制标题时做英文名 → 中文名映射；未收录的名字（用户自建
   实例）原样显示。Sink 菜单里的输出流名与 provider 名也走同一映射，配置里保存的仍是英文名。

## 修改文案后如何更新字体

改了任何中文界面文本后，重新生成字体子集与字形范围，然后提交两个产物：

```bash
python scripts/zh/gen_cjk_glyphs.py
```

脚本会下载 Noto Sans SC 可变字体（缓存到 `scripts/zh/.fonts/`）、在 `wght=400` 处实例化、
按当前源码用字 + ImGui 常用字集做子集，并重写 `core/src/gui/cjk_glyphs.h`。
需要 Python 3 与 `fonttools`（`pip install fonttools`）。

菜单标题表由 `scripts/zh/menu_names.json` 生成：

```bash
python scripts/zh/gen_menu_names.py
```

## 上游同步

本地化以替换字符串字面量为主，与上游合并时这些行容易冲突，按普通冲突处理即可；
字体、`style.cpp` 的字体加载、`CMakeLists.txt` 的 `/utf-8` 与菜单映射属于独立新增，
冲突面很小。新增可翻译文案时建议顺带运行上面的字形生成脚本，避免出现缺字方块。
