/*
 * test.c
 *
 *  Created on: 2017年3月27日
 *      Author: xiechunping
 */
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<errno.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>

#define MAXLINE 4096

int main(int argc, char** argv) {
	int sockfd, n, rec_len;
	char recvline[4096], sendline[4096];
	char *sp;
	char buf[MAXLINE];
	struct sockaddr_in servaddr;

	if (argc != 2) {
		printf("usage: ./client <ipaddress>\n");
		exit(0);
	}

	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		printf("create socket error: %s(errno: %d)\n", strerror(errno), errno);
		exit(0);
	}

	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(8888);
	if (inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0) {
		printf("inet_pton error for %s\n", argv[1]);
		exit(0);
	}

	if (connect(sockfd, (struct sockaddr*) &servaddr, sizeof(servaddr)) < 0) {
		printf("connect error: %s(errno: %d)\n", strerror(errno), errno);
		exit(0);
	}

	printf("send msg to server: \n");
	sp = "d2cb80e479eaed328474d28364094cf2";
	//fgets(sendline, 4096, stdin);
	if (send(sockfd, sp, strlen(sp), 0) < 0) {
		printf("send msg error: %s(errno: %d)\n", strerror(errno), errno);
		exit(0);
	}
	
	if ((rec_len = recv(sockfd, buf, MAXLINE, 0)) == -1) {
		perror("recv error");
		exit(1);
	}
	buf[rec_len] = '\0';
	if(!strcmp(buf,"ok")){
		printf("Shakehand success!");
	}else{
		printf("Shakehand failed!");
	}
	printf("Received : %s ", buf);
	close(sockfd);
	exit(0);
}
