/*
 * skai.h — 连接 AI 服务 skaid 的客户端协议（命令行 ai 和图形应用 skai 共用）
 *
 * skaid 是系统里的“AI 服务”：它在 Unix 域套接字 SKAI_SOCKET_PATH 上等待客户端。
 * 每个连接就是一段独立的对话（连接断开，对话就结束）。双方都按“行”说话，每行一条消息：
 *
 *     命令 参数\n
 *
 * 参数里的换行写成 \n、反斜杠写成 \\（skai_escape / skai_unescape），这样一条消息永远只占一行。
 *
 * 客户端 → skaid：
 *     ASK <问题>          提一个问题（或者下一步的要求）；AI 可能会调用工具完成任务
 *     RESET               清空这段对话，从头开始
 *     CANCEL              停止正在进行的回答
 *     STATUS              查询状态（skaid 回一条 STATUS）
 *
 * skaid → 客户端：
 *     STATUS <状态> <模型> <说明>   状态：ready 可以使用 / nokey 网页上还没填 API 密钥 /
 *                                 nobridge 没有找到 AI 桥（不在网页版里）/ waiting 正在联系网页
 *     BUSY <0|1>                  开始/结束处理一个问题
 *     THINK [思考摘要片段]          模型在思考（参数为空 = 刚开始思考；之后可能送来思考过程的摘要）
 *     TEXT <文字片段>              回答的文字（流式，一段段到达）
 *     TOOL <工具名> <参数摘要>       AI 正在调用一个工具（运行命令、读写文件、打开应用……）
 *     RESULT <ok|err> <结果摘要>    工具执行完了
 *     USAGE <输入 token> <输出 token>   这一轮用掉的 token 数（累计）
 *     DONE <结束原因>               这个问题处理完了（end_turn、max_tokens、cancelled……）
 *     ERROR <消息>                 出错了（之后仍会发 DONE error）
 */
#ifndef SKAI_H
#define SKAI_H

#include <stddef.h>

#define SKAI_SOCKET_PATH "/run/sketch/ai.sock"
#define SKAI_SOCKET_ENV  "SKAI_SOCKET"          /* 测试时可以换一个套接字路径 */
#define SKAI_LINE_MAX    (256 * 1024)            /* 一行最长多少字节（超过就截断） */

/* 连接 skaid，返回 fd（失败 -1，errno 说明原因） */
int   skai_connect(void);
/* 转义 / 反转义，返回 malloc 的新字符串 / 原地修改 */
char *skai_escape(const char *s);
void  skai_unescape(char *s);
/* 发送一条消息：cmd + 空格 + 转义后的 arg（arg 可为 NULL）+ 换行。成功 0 */
int   skai_send(int fd, const char *cmd, const char *arg);

/* 按行读取：fill 读入现有数据（返回读到的字节数，0 = 对方关闭，-1 = 出错/EAGAIN），
 * next 取出下一整行（malloc，去掉 \n，未反转义），没有完整的一行时返回 NULL */
typedef struct {
    char  *d;
    size_t len, cap;
} skai_reader;
long  skai_reader_fill(skai_reader *r, int fd);
char *skai_reader_next(skai_reader *r);
void  skai_reader_free(skai_reader *r);

/* 把一行拆成命令和参数：line 被原地修改，*arg 指向参数（已反转义，可能是空串） */
const char *skai_split(char *line, char **arg);

#endif /* SKAI_H */
