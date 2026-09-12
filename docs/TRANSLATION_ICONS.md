# 翻译源图标

添加窗口、已添加源列表及翻译浮窗的结果卡片使用同一套内嵌矢量路径，运行时不读取图片或 SVG 文件。GDI+ 抗锯齿绘制，复用截图工具栏的 SVG path 解析器，随窗口 DPI 缩放。

路径来源： [Simple Icons 11.15.0](https://github.com/simple-icons/simple-icons/tree/11.15.0)，项目采用 [CC0-1.0](https://github.com/simple-icons/simple-icons/blob/11.15.0/LICENSE.md)。服务名称和商标属于相应权利人，用于标识用户配置的服务，不表示官方合作或背书。

- [OpenAI](https://github.com/simple-icons/simple-icons/blob/11.15.0/icons/openai.svg)：黑色结形标识。
- [DeepL](https://github.com/simple-icons/simple-icons/blob/11.15.0/icons/deepl.svg)：仅保留左侧六角形标识，去除字标路径。
- [Microsoft Azure](https://github.com/simple-icons/simple-icons/blob/11.15.0/icons/microsoftazure.svg)：蓝色 Azure 标识；对应本项目的 Azure Translator 接口。
- [百度](https://github.com/simple-icons/simple-icons/blob/11.15.0/icons/baidu.svg)：蓝色百度标识。

源码：`src/translation/source_icons.cpp`。没有复制 TTime 的品牌素材或界面源码。

密钥显示／隐藏使用用户提供的 `240显示、可见.svg`、`241隐藏、不可见.svg` 中的全部 path 数据，直接内嵌在同一源码中。密码遮蔽时按钮显示睁眼，点击显示密钥；明文时显示划线眼睛，点击隐藏密钥，悬停提示相应动作。原 SVG 文件不复制到源码资源、运行目录或安装包。
