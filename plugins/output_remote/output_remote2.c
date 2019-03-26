/*
  远程视频推流，用于将图片流推送到远程服务器，从而实现远程监控
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <resolv.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>

#include <dirent.h>

#include "../../utils.h"
#include "../../mjpg_streamer.h"

#define OUTPUT_PLUGIN_NAME "REMOTE output plugin"

static pthread_t worker;
static globals *pglobal;
static int fd, delay, max_frame_size;
static char *folder = "/tmp";
static unsigned char *frame = NULL;
static char *command = NULL;
static int input_number = 0;

// UDP port
static int port = 0;

/******************************************************************************
描述.: 打印帮助信息
输入.: -
返回: -
******************************************************************************/
void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
            " Help for output plugin..: "OUTPUT_PLUGIN_NAME"\n" \
            " ---------------------------------------------------------------\n" \
            " The following parameters can be passed to this plugin:\n\n" \
            " [-f | --folder ]........: folder to save pictures\n" \
            " [-d | --delay ].........: delay after saving pictures in ms\n" \
            " [-c | --command ].......: execute command after saveing picture\n" \
            " [-p | --port ]..........: UDP port to listen for picture requests. UDP message is the filename to save\n\n" \
            " [-i | --input ].......: read frames from the specified input plugin (first input plugin between the arguments is the 0th)\n\n" \
            " ---------------------------------------------------------------\n");
}

/******************************************************************************
描述: 清理分配的资源
输入: 参数指针
返回: -
******************************************************************************/
void worker_cleanup(void *arg)
{
    static unsigned char first_run = 1;

    if(!first_run) {
        DBG("already cleaned up resources\n");
        return;
    }

    first_run = 0;
    OPRINT("cleaning up resources allocated by worker thread\n");

    if(frame != NULL) {
        free(frame);
    }
    close(fd);
}

/******************************************************************************
描述: 此为工作主线程，此函数永久循环，用于将图片流推送到远程服务器
输入: 参数指针
返回:
******************************************************************************/
void *worker_thread(void *arg)
{
    int ok = 1, frame_size = 0, rc = 0;
    char buffer1[1024] = {0};
    unsigned char *tmp_framebuffer = NULL;

    /* 设置资源清理函数句柄*/
    pthread_cleanup_push(worker_cleanup, NULL);

    // 设置UDP服务器数据结构---------------------------------------
    if(port <= 0) {
        OPRINT("a valid UDP port must be provided\n");
        return NULL;
    }
    struct sockaddr_in addr;
    int sd;
    int bytes;
    unsigned int addr_len = sizeof(addr);
    char udpbuffer[1024] = {0};
    sd = socket(PF_INET, SOCK_DGRAM, 0);
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if(bind(sd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
        perror("bind");
    // -----------------------------------------------------------

    while(ok >= 0 && !pglobal->stop) {
        DBG("waiting for a UDP message\n");

        // UDP接收 ---------------------------------------------
        memset(udpbuffer, 0, sizeof(udpbuffer));
        bytes = recvfrom(sd, udpbuffer, sizeof(udpbuffer), 0, (struct sockaddr*)&addr, &addr_len);
        // ---------------------------------------------------------


        //阻止其它线程访问全局缓存
        DBG("waiting for fresh frame\n");
        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_wait(&pglobal->in[input_number].db_update, &pglobal->in[input_number].db);

        /* 读取缓存帧大小 */
        frame_size = pglobal->in[input_number].size;

        /* 检查用于帧缓存是否足够大，否则增加并分配空间*/
        if(frame_size > max_frame_size) {
            DBG("increasing buffer size to %d\n", frame_size);

            max_frame_size = frame_size + (1 << 16);//frame_size + 65536
            if((tmp_framebuffer = realloc(frame, max_frame_size)) == NULL) {
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                LOG("not enough memory\n");
                return NULL;
            }

            frame = tmp_framebuffer;
        }

        /* 拷贝帧数据到本地缓存*/
        memcpy(frame, pglobal->in[input_number].buf, frame_size);

        /* 再次允许其它线程访问全局缓存 */
        pthread_mutex_unlock(&pglobal->in[input_number].db);

        /* 如果文件名来自UDP消息，则保存以此为文件名保存*/
        if(strlen(udpbuffer) > 0) {
            DBG("writing file: %s\n", udpbuffer);

            /* 打开文件写入，路径必须预先存在 */
            if((fd = open(udpbuffer, O_CREAT | O_RDWR | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) {
                OPRINT("could not open the file %s\n", udpbuffer);
                return NULL;
            }

            /* 保存图片到文件 */
            if(write(fd, frame, frame_size) < 0) {
                OPRINT("could not write to file %s\n", udpbuffer);
                perror("write()");
                close(fd);
                return NULL;
            }

            close(fd);
        }

        // 发回文件名消息给客户端
        sendto(sd, udpbuffer, bytes, 0, (struct sockaddr*)&addr, sizeof(addr));

        /* 如果设置了命令行，则进行调用，并以文件名作为参数 */
        if(command != NULL) {
            memset(buffer1, 0, sizeof(buffer1));

            /* 把文件名作为参数传递给命令行 */
            snprintf(buffer1, sizeof(buffer1), "%s \"%s\"", command, udpbuffer);
            DBG("calling command %s", buffer1);

            /* 同时设置文件名作为环境变量 */
            if((rc = setenv("MJPG_FILE", udpbuffer, 1)) != 0) {
                LOG("setenv failed (return value %d)\n", rc);
            }

            /* 执行命令 */
            if((rc = system(buffer1)) != 0) {
                LOG("command failed (return value %d)\n", rc);
            }
        }

        /* 如果设置了，延迟执行 */
        if(delay > 0) {
            usleep(1000 * delay);
        }
    }

    // 关闭udp端口
    if(port > 0)
        close(sd);

    /* 清理线程 */
    pthread_cleanup_pop(1);

    return NULL;
}


/*** 插件接口函数 ***/
/******************************************************************************
描述: 此函数首先别调用，传递参数字符串初始化插件
输入: 输入参数指针
返回: 如果成功返回0，否则返回非0
******************************************************************************/
int output_init(output_parameter *param)
{
    int i;

    delay = 0;

    param->argv[0] = OUTPUT_PLUGIN_NAME;

    /* 显示所有的参数用于调试 */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }

    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"h", no_argument, 0, 0
            },
            {"help", no_argument, 0, 0},
            {"f", required_argument, 0, 0},
            {"folder", required_argument, 0, 0},
            {"d", required_argument, 0, 0},
            {"delay", required_argument, 0, 0},
            {"c", required_argument, 0, 0},
            {"command", required_argument, 0, 0},
            {"p", required_argument, 0, 0},
            {"port", required_argument, 0, 0},
            {"i", required_argument, 0, 0},
            {"input", required_argument, 0, 0},
            {0, 0, 0, 0}
        };

        c = getopt_long_only(param->argc, param->argv, "", long_options, &option_index);

        /* no more options to parse */
        if(c == -1) break;

        /* unrecognized option */
        if(c == '?') {
            help();
            return 1;
        }

        switch(option_index) {
            /* h, help */
        case 0:
        case 1:
            DBG("case 0,1\n");
            help();
            return 1;
            break;

            /* f, folder */
        case 2:
        case 3:
            DBG("case 2,3\n");
            folder = malloc(strlen(optarg) + 1);
            strcpy(folder, optarg);
            if(folder[strlen(folder)-1] == '/')
                folder[strlen(folder)-1] = '\0';
            break;

            /* d, delay */
        case 4:
        case 5:
            DBG("case 4,5\n");
            delay = atoi(optarg);
            break;

            /* c, command */
        case 6:
        case 7:
            DBG("case 6,7\n");
            command = strdup(optarg);
            break;
            /* p, port */
        case 8:
        case 9:
            DBG("case 8,9\n");
            port = atoi(optarg);
            break;
            /* i, input */
        case 10:
        case 11:
            DBG("case 10,11\n");
            input_number = atoi(optarg);
            break;
        }
    }

    pglobal = param->global;
    if(!(input_number < pglobal->incnt)) {
        OPRINT("ERROR: the %d input_plugin number is too much only %d plugins loaded\n", input_number, pglobal->incnt);
        return 1;
    }
    OPRINT("input plugin.....: %d: %s\n", input_number, pglobal->in[input_number].plugin);
    OPRINT("output folder.....: %s\n", folder);
    OPRINT("delay after save..: %d\n", delay);
    OPRINT("command...........: %s\n", (command == NULL) ? "disabled" : command);
    if(port > 0) {
        OPRINT("UDP port..........: %d\n", port);
    } else {
        OPRINT("UDP port..........: %s\n", "disabled");
    }
    return 0;
}

/******************************************************************************
描述: 调用此函数停止工作线程
输入值: -
返回值: 0
******************************************************************************/
int output_stop(int id)
{
    DBG("will cancel worker thread\n");
    pthread_cancel(worker);
    return 0;
}

/******************************************************************************
描述: 调用此函数创建并开始一个工作线程
输入值: -
返回值: 0
******************************************************************************/
int output_run(int id)
{
    DBG("launching worker thread\n");
    pthread_create(&worker, 0, worker_thread, NULL);
    pthread_detach(worker);
    return 0;
}


