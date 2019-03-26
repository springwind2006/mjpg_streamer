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
#define RECV_MAXLINE 4096

static pthread_t worker;
static globals *pglobal;
static int max_frame_size;
static unsigned char *frame = NULL;
static int input_number = 0;
static char *host = "180.76.169.237";
static int port = 8889;
static int delay=0;

/******************************************************************************
 描述.: 打印帮助信息
 输入.: -
 返回: -
 ******************************************************************************/
void help(void) {
	fprintf(stderr,
			" ---------------------------------------------------------------\n"
					" Help for output plugin..: "OUTPUT_PLUGIN_NAME"\n"
			" ---------------------------------------------------------------\n"
			" The following parameters can be passed to this plugin:\n\n"
			" [-h | --host ]........: the remote host ip address\n"
			" [-p | --port ]..........: the romote host port\n\n"
			" [-d | --delay ].........: delay after send pictures in ms\n"
			" [-i | --input ].......: read frames from the specified input plugin (first input plugin between the arguments is the 0th)\n\n"
			" ---------------------------------------------------------------\n");
}

/******************************************************************************
 描述: 清理分配的资源
 输入: 参数指针
 返回: -
 ******************************************************************************/
void worker_cleanup(void *arg) {
	static unsigned char first_run = 1;
	if (!first_run) {
		DBG("already cleaned up resources\n");
		return;
	}
	first_run = 0;
	OPRINT("cleaning up resources allocated by worker thread\n");
	if (frame != NULL) {
		free(frame);
	}
}

/******************************************************************************
 描述: 此为工作主线程，此函数永久循环，用于将图片流推送到远程服务器
 输入: 参数指针
 返回:
 ******************************************************************************/
void *worker_thread(void *arg) {
	int ok = 1, frame_size = 0, rc = 0;
	char buffer1[1024] = { 0 };
	unsigned char *tmp_framebuffer = NULL;

	/* 设置资源清理函数句柄*/
	pthread_cleanup_push(worker_cleanup, NULL);

	// 设置UDP服务器数据结构---------------------------------------
	if (port <= 0) {
		OPRINT("a valid TCP port must be provided\n");
		return NULL;
	}
	
	//初始化套接字变量
	struct sockaddr_in addr;
	int sockfd;
	int bytes,rec_len;
	unsigned int addr_len = sizeof(addr);
	char udpbuffer[1024] = { 0 };
	char *auth_key="d2cb80e479eaed328474d28364094cf2";
	char recv_buf[RECV_MAXLINE];
	
	//创建套接字
	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		OPRINT("create socket error\n");
		return NULL;
	}
	
	//设置远程服务器地址和端口
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
		OPRINT("inet_pton error for %s\n", host);
		return NULL;
	}
	
	//连接到远程服务器
	if (connect(sockfd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
		OPRINT("connect error: %s(errno: %d)\n", strerror(errno), errno);
		return NULL;
	}
	
	//发送认证信息到远程服务器
	OPRINT("send config message to server: \n");
	if (send(sockfd, auth_key, strlen(auth_key), 0) < 0) {
		OPRINT("send msg error: %s(errno: %d)\n", strerror(errno), errno);
		return NULL;
	}
	
	if ((rec_len = recv(sockfd, recv_buf, RECV_MAXLINE, 0)) == -1) {
		OPRINT("recv config result error");
		return NULL;
	}
	recv_buf[rec_len] = '\0';
	if(!strcmp(recv_buf,"ok")){
		OPRINT("Server config success!");
	}else{
		OPRINT("Server config failed!");
		return NULL;
	}
	
	// -----------------------------------------------------------

	while (ok >= 0 && !pglobal->stop) {
		//阻止其它线程访问全局缓存
		DBG("waiting for fresh frame\n");
		pthread_mutex_lock(&pglobal->in[input_number].db);
		pthread_cond_wait(&pglobal->in[input_number].db_update,&pglobal->in[input_number].db);

		/* 读取缓存帧大小 */
		frame_size = pglobal->in[input_number].size;

		/* 检查用于帧缓存是否足够大，否则增加并分配空间*/
		if (frame_size > max_frame_size) {
			DBG("increasing buffer size to %d\n", frame_size);
			max_frame_size = frame_size + (1 << 16); //frame_size + 65536
			if ((tmp_framebuffer = realloc(frame, max_frame_size)) == NULL) {
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

		// 发送图片流到服务器
		send(sockfd, frame, strlen(frame), 0);

		/* 如果设置了，延迟执行 */
		if (delay > 0) {
			usleep(1000 * delay);
		}
	}

	// 关闭udp端口
	if (port > 0){
		close(sockfd);
	}

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
int output_init(output_parameter *param) {
	int i;

	delay = 0;

	param->argv[0] = OUTPUT_PLUGIN_NAME;

	/* 显示所有的参数用于调试 */
	for (i = 0; i < param->argc; i++) {
		DBG("argv[%d]=%s\n", i, param->argv[i]);
	}

	reset_getopt();
	while (1) {
		int option_index = 0, c = 0;
		static struct option long_options[] = {
				{ "q", no_argument, 0, 0 }, 
				{"help", no_argument, 0, 0 }, 
				{ "h", required_argument, 0, 0 }, 
				{ "host", required_argument, 0, 0 }, 
				{ "p", required_argument, 0,0 }, 
				{ "port", required_argument, 0, 0 }, 
				{ "d",no_argument, 0, 0 }, 
				{ "delay", no_argument, 0, 0 },
				{ "i", no_argument, 0, 0 }, 
				{ "input", no_argument, 0, 0 }, 
				{ 0,0, 0, 0 } 
		};

		c = getopt_long_only(param->argc, param->argv, "", long_options,
				&option_index);

		/* no more options to parse */
		if (c == -1)
			break;

		/* unrecognized option */
		if (c == '?') {
			help();
			return 1;
		}

		switch (option_index) {
		/* q, help */
		case 0:
		case 1:
			DBG("case 0,1\n");
			help();
			return 1;
			break;
			/* h, host */
		case 2:
		case 3:
			DBG("case 2,3\n");
			host = malloc(strlen(optarg) + 1);
			strcpy(host, optarg);
			break;
			/* p, port */
		case 4:
		case 5:
			DBG("case 4,5\n");
			port = atoi(optarg);
			break;
			/* d, delay */
		case 6:
		case 7:
			DBG("case 6,7\n");
			delay = atoi(optarg);
			break;
			/* i, input */
		case 8:
		case 9:
			DBG("case 8,9\n");
			input_number = atoi(optarg);
			break;
		}
	}

	pglobal = param->global;
	if (!(input_number < pglobal->incnt)) {
		OPRINT("ERROR: the %d input_plugin number is too much only %d plugins loaded\n",input_number, pglobal->incnt);
		return 1;
	}
	OPRINT("input plugin.....: %d: %s\n", input_number,pglobal->in[input_number].plugin);
	OPRINT("remote host.....: %s\n", host);
	OPRINT("remote port.....: %d\n", port);
	OPRINT("delay after send..: %d\n", delay);
	return 0;
}

/******************************************************************************
 描述: 调用此函数停止工作线程
 输入值: -
 返回值: 0
 ******************************************************************************/
int output_stop(int id) {
	DBG("will cancel worker thread\n");
	pthread_cancel(worker);
	return 0;
}

/******************************************************************************
 描述: 调用此函数创建并开始一个工作线程
 输入值: -
 返回值: 0
 ******************************************************************************/
int output_run(int id) {
	DBG("launching worker thread\n");
	pthread_create(&worker, 0, worker_thread, NULL);
	pthread_detach(worker);
	return 0;
}

