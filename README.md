# Lab 2：符号执行

用 Z3 为小语言 MiniC 写一个符号执行工具：给出让程序失败的输入，或证明程序安全。

**请先阅读 `lab-manual.pdf`（实验手册），MiniC 语言规范在手册附录 A。**

```sh
pip install -r requirements.txt                     # 安装 Z3（自带 C++ 头文件和库）
make                                                # 编译 build/see、build/run、build/grade
make grade                                          # 在公开题上评分，按模块给出得分
make grade BLOCK=loop                               # 只评测一个模块的题
./build/see tasks/public/p02_warmup_xor.mc          # 运行你的工具
./build/run tasks/public/p02_warmup_xor.mc '{"x": 1, "y": 2}'   # 用解释器运行一个输入
make handin                                         # 打包 see/ 为 handin.zip
```

| 路径                             | 内容                                                          |
| -------------------------------- | ------------------------------------------------------------- |
| `see/core.cpp`                   | **你要填的空**，分 8 个功能模块，见手册第 4.3 节              |
| `see/engine.h`、`see/engine.cpp` | 符号执行的框架：状态、语句和表达式的遍历                      |
| `see/main.cpp`                   | 入口 `./build/see TASK.mc`：读题、运行、验证反例、输出答案    |
| `minic/`                         | 语言前端、参考解释器、评分逻辑（评测使用自己的副本）          |
| `tasks/public/`                  | 23 道公开题及标准答案（`expected.json` 标出每题所属模块）     |
| `tasks/blocks.json`              | 功能模块的依赖关系与分值                                      |
| `tools/`                         | `build/run`（运行单个输入）和 `build/grade`（本地评测）的源码 |
