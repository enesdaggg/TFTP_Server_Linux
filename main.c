/*
 ============================================================================
 Name        : main.c
 Author      : @enesdaggg
 Description : TFTP Server
 ============================================================================
 */

/* Main fonksiyonunda, programın başlangıcında argüman alacağı belirtilmiştir. Bunlar:
 * "int argc": Program yürütülmeye başladığındaki argüman sayısı
 * "char *argv[]": Argümanların her birinin tutulduğu dizi olup elemanları:
 * argv[0]: "./<programın_adı>"
 * argv[1]: Server'ın dosya alışverişinde kullanacağı ana dizin
 * argv[2]: Port numarası
 * */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

//Önişlemci Direktifleri
#define	SOCKET_DOMAIN_ADDR		INADDR_ANY	// Soketin bağlanacağı protokol ailesinin seçimi (Local için: AF_LOCAL)
#define	RECEIVE_TIMEOUT_SEC 	5 			// Saniye cinsinden zaman aşımı seçimi
#define	RECEIVE_TIMEOUT_USEC	0			// Mikrosaniye cinsinden zaman aşımı seçimi
#define	TFTP_DATA_DEFAULT		512			// TFTP paketlerinin barındırabileceği maksimum veri boyutu (bayt)
#define	TFTP_DATA_MINIMUM		4			// TFTP Data paketlerinin sahip olabileceği en düşük boyut (bayt cinsinden)
#define	TFTP_RETRY_NUMBER		5 			// Paket transferinde problem yaşandığında yapılacak maksimum tekrar deneme sayısı
#define	EXIT_SUCCESS			0			// Başarılı sonlandırmayı bildirmek için exit() fonksiyonunda kullanılır
#define	EXIT_FAILURE			1			// Anormal sonlandırmayı bildirmek için exit() fonksiyonunda kullanılır

// TFTP Paketleri İçin Opcode Tanımlamaları
enum opcode {
	RRQ = 1,	// Read Request (Okuma İsteği)
	WRQ,		// Write Request (Yazma İsteği)
	DATA,	// Data (Veri)
	ACK,		// Acknowledgment (Alındı, Geri Bildirim)
	ERROR,	// Hata
};

//TFTP Transfer Modu Tanımlamaları
enum transferMode {
	NETASCII = 1,
	OCTET,
	MAIL,
};

//TFTP Paket Yapılarında Kullanılacak Değişkenler İçin Veri Türü Tanımlamaları
typedef union {

	uint16_t opcode;					// İşlem Kodu (2 bayt)

	struct {
		uint16_t opcode;				// İşlem Kodu (2 bayt)
		char fileName_and_mode[514];		// Dosya Adı, 0 (1 bayt), Transfer Modu, 0 (1 bayt)
	} request; // RRQ ve WRQ paketlerinde kullanılacak veriler

	struct {
		uint16_t opcode;				// İşlem Kodu (2 bayt)
		uint16_t blockNumber;
		char data[TFTP_DATA_DEFAULT];
	} data; // DATA paketlerinde kullanılacak veriler

	struct {
		uint16_t opcode;
		uint16_t blockNumber;
	} ack; // ACK paketlerinde kullanılacak veriler

	struct {
		uint16_t opcode;
		uint16_t errorCode;
		char errorMessage[TFTP_DATA_DEFAULT];
	} error; // ERROR paketlerinde kullanılacak veriler

} tftpMessage;

char *fileBaseDirectory;

// Soket Kurulumu
int16_t tftp_socket_start(void)
{
	int16_t socketFilDes = -1; 	// Socket kimliği, socket() fonksiyonunun hata belirttiği değerle tanımlandı

	struct protoent *protocole;	// Protokol veritabanı bilgilerinin yer alacağı struct'ın oluşturulması

	struct timeval tv;				// Zaman değişkenlerinin tutulabileceği struct'ın oluşturulması
	tv.tv_sec  = RECEIVE_TIMEOUT_SEC;	// Zaman aşımı için saniye cinsinden sürenin tanımlanması
	tv.tv_usec = RECEIVE_TIMEOUT_USEC;	// Zaman aşımı için mikrosaniye cinsinden sürenin tanımlanması

	// Protokol Seçimi (UDP)
	if ((protocole = getprotobyname("udp")) == 0) {		  	              // Hata Kontrolü: Protokol seçimi hatalı yapıldığında
		fprintf(stderr, "Server: tftp_socket_start(): getprotobyname() error\n"); // getprotobyname() fonksiyonu 0 değerini döndürür
		exit(EXIT_FAILURE);
	}

	// Server-Client İletişimi İçin Soket Kurulumu
	// AF_INET: İletişim etki alanının IPv4 olacağını belirtir. Local istenseydi AF_UNIX olurdu.
	// SOCK_DGRAM: İletişimin anlamsallığının(semantic), bağlantısız çalışıp datagramları desteklediğini belirtir
	// protocole->p_proto: Protokol türünü belirtir. 0 yazılabilir.
	if ((socketFilDes = socket(AF_INET, SOCK_DGRAM, protocole->p_proto)) == -1) { // Hata Kontrolü: socket() fonksiyonu hata durumunda
		perror("Server: tftp_socket_start(): socket()"); 								  // -1 değeri döndürür
		exit(EXIT_FAILURE);
	}

	// Soket İçin Zaman Aşımı Ayarının Yapılması
	// SOL_SOCKET: Level argümanı olup soket seviyesinde işlem yapıldığını belirtir
	// SO_RCVTIMEO seçeneği, bir giriş işleminin tamamlanasaya kadar beklenecek maksimum süreyi belirten zaman aşımı değeridir
	if(setsockopt(socketFilDes, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) { // Hata Kontrolü: setsockopt() fonksiyonu başarılı
		perror("Server: tftp_socket_start(): setsockopt()");							 // çalıştığında 0; hata durumunda -1 döndürür.
		close(socketFilDes);	// Soketin sonlandırılması
		exit(EXIT_FAILURE);
	}

	return socketFilDes;
}

// Client'tan paket almayı sağlayan fonksiyon. Gönderdiği verinin boyutunu döndürür.
signed int tftp_receive_message(signed short socketID, tftpMessage *message, struct sockaddr_in *socket, socklen_t *socketLength)
{
	signed int receivedMessageSize;		// Gelen verinin, bayt cinsinden boyutunun kaydedileceği değişken

	if (((receivedMessageSize = recvfrom(socketID, message, sizeof(*message), 0, (struct sockaddr *) socket, socketLength)) < 0) && (errno != EAGAIN)) {
		perror("SERVER ERROR: recvfrom()!"); //Alınan son hatanın açıklamasını yazdırır (Hata, errno değişkeninde saklanır)
	}

	return receivedMessageSize;		// Gelen verinin, bayt cinsinden boyutunun geri döndürülmesi
}

// Client'a DATA paketinin gönderimini sağlayan olan fonksiyon.  Gönderdiği verinin boyutunu döndürür.
signed int tftp_send_data(int socketID, uint16_t blockNumber, uint8_t *data, ssize_t dataLength, struct sockaddr_in *socket, socklen_t socketLength)
{
	tftpMessage message;	// tftpMessage bileşimindeki veri türlerinin, fonksiyon içerisinde kullanıma hazır hale getirilmesi
	signed int sentSize;	// Gönderilen verinin, bayt cinsinden boyutunun kaydedileceği değişken

	message.opcode = htons(DATA); 				// Opcode verisinin, bellekte, ağ bayt sıralamasına göre DATA olarak tutulmasını sağlar
	message.data.blockNumber = htons(blockNumber); 	// Blok numarası verisinin, bellekte ağ bayt sıralamasına göre (MSB) tutulmasını sağlar
	memcpy(message.data.data, data, dataLength); 	// dataLength kadar uzunluktaki data bellek verisi, union data.data'ya kaydedildi

	if ((sentSize = sendto(socketID, &message, dataLength + TFTP_DATA_MINIMUM, 0, (struct sockaddr *) socket, socketLength)) < 0) { // Paket gönderimi
		perror("Server: sendto()");	// Paket gönderiminin hata kontrolü. Alınan hatanın açıklamasını yazdırır. (Hata, errno değişkeninde saklanır)
	}

	return sentSize;	// Gönderilen verinin, bayt cinsinden boyutunun geri döndürülmesi
}

// Client'a ACK paketinin gönderimini sağlayan fonksiyon. Gönderdiği verinin boyutunu döndürür.
signed short tftp_send_ack(int socketID, uint16_t blockNumber, struct sockaddr_in *socket, socklen_t socketLength)
{
	tftpMessage message;	// tftpMessage bileşimindeki veri türlerinin, fonksiyon içerisinde kullanıma hazır hale getirilmesi
	signed short sentSize;	// Gönderilen verinin, bayt cinsinden boyutunun kaydedileceği değişken

	message.opcode = htons(ACK); 					// Opcode verisinin, bellekte, ağ bayt sıralamasına göre ACK olarak tutulmasını sağlar
	message.ack.blockNumber = htons(blockNumber);	// Blok numarası verisinin, bellekte ağ bayt sıralamasına göre (MSB) tutulmasını sağlar

	if ((sentSize = sendto(socketID, &message, sizeof(message.ack), 0, (struct sockaddr *) socket, socketLength)) < 0) { // Paket gönderimi
		perror("Server: sendto()"); // Paket gönderiminin hata kontrolü. Alınan hatanın açıklamasını yazdırır. (Hata, errno değişkeninde saklanır)
	}

	return sentSize;	// Gönderilen verinin, bayt cinsinden boyutunun geri döndürülmesi
}

// Client'a ERROR paketinin gönderimini sağlayan fonksiyon. Gönderdiği verinin boyutunu döndürür.
signed short tftp_server_send_error(int socketID, int errorCode, char *errorString, struct sockaddr_in *socket, socklen_t socketLength)
{
	tftpMessage message;	// tftpMessage bileşimindeki veri türlerinin, fonksiyon içerisinde kullanıma hazır hale getirilmesi
	signed short sentSize;	// Gönderilen verinin, bayt cinsinden boyutunun kaydedileceği değişken

	if(strlen(errorString) >= TFTP_DATA_DEFAULT) { // Hata mesajı boyutunun sınırının aşılma durumunun kontrolü
		fprintf(stderr, "Server: tftpServerSendError(): Error message is too long!\n");
		return -1;
	}

	message.opcode = htons(ERROR);				// Opcode verisinin bellekte ağ bayt sıralamasına göre ERROR olarak tutulmasını sağlar
	message.error.errorCode = htons(errorCode);		// Hata kodu numarasının, bellekte tutulan değişkene ağ bayt sıralamasına göre kaydı
	strcpy(message.error.errorMessage, errorString);	// Hata mesajının, bellekteki errorMessage değişkenine kopyalanması

	if ((sentSize = sendto(socketID, &message, strlen(errorString) + 1, 0, (struct sockaddr *) socket, socketLength)) < 0)	// Paket gönderimi
	{																								// ve hata kontrolü
		perror("SERVER: sendto()");	// Paket gönderiminin hata kontrolü. Alınan hatanın açıklamasını yazdırır. (Hata, errno değişkeninde saklanır)
	}

	return sentSize;	// Gönderilen verinin, bayt cinsinden boyutunun geri döndürülmesi
}

// WRQ ve RRQ Paketlerinin İşlendiği Fonksiyon
void tftp_server_handle_request(tftpMessage *message, signed int messageByte, struct sockaddr_in *client_socket, socklen_t socketLength)
{
	int16_t socketFilDes;	// Soketin dosya betimleyicisi (Socket File Descriptor)
	uint16_t opcode;		// İşlem kodunu belirten değişken (opcode)
	char mode; 			// Transfer modunu belirten değişken
	char *mode_s;			// Transfer modunun string olarak tutulacağı değişken
	char *fileName;		// ? Aktarılacak dosya adı ve transfer modunu barındıracak değişken
	char *fileNameEnd;		// ? Aktarılacak dosya adını barındıracak değişken

	FILE *fd;				// Transferi gerçekleşmekte olan dosya işlemlerinin takibi için kullanılan dosya struct'ı

	// Soket kurulumu ve dosya betimleyicisinin kaydedilmesi
	socketFilDes = tftp_socket_start();

	// Client'ın gönderdiği paketin çözümleme işlemleri
	fileName = message->request.fileName_and_mode;		// fileName'in işaret ettiği adresten itibaren 514 bayt'lık yer ayrıldı
	fileNameEnd = &fileName[messageByte - 2 - 1];

	if (*fileNameEnd != '\0') { 	// Hata Kontrolü: Dosya adının ve transfer modunun geçerliliğinin kontrolü
		printf("%s.%u: Invalid fileName or mode\n", inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
		tftp_server_send_error(socketFilDes, 0, "Invalid fileName or mode", client_socket, socketLength);
		exit(EXIT_FAILURE);
	}

	mode_s = strchr(fileName, '\0') + 1;	// Transfer modunun olduğu adresin bulunup mode_s değişkenine kaydedilmesi (netascii, octet, mail)

	if (mode_s > fileNameEnd) {
		printf("%s.%u: transfer mode not specified\n",
				inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
		tftp_server_send_error(socketFilDes, 0, "Transfer mode not specified", client_socket, socketLength);
		exit(EXIT_FAILURE);
	}

	if(strncmp(fileName, "../", 3) == 0 || strstr(fileName, "/../") != NULL ||
			(fileName[0] == '/' && strncmp(fileName, fileBaseDirectory, strlen(fileBaseDirectory)) != 0)) {
		printf("%s.%u: fileName outside base directory\n",
				inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
		tftp_server_send_error(socketFilDes, 0, "FileName outside base directory", client_socket, socketLength);
		exit(EXIT_FAILURE);
	}

	opcode = ntohs(message->opcode);					// İşlem kodu kaydı
	fd = fopen(fileName, opcode == RRQ ? "r" : "w"); 	/* Dosya işlemleri için Client'tan gelen pakete göre izin alınır (Okuma/Yazma)
														Akışı kontrol eden nesneye işaretçi döner */
	if (fd == NULL) {
		perror("server: fopen()");
		tftp_server_send_error(socketFilDes, errno, strerror(errno), client_socket, socketLength);
		exit(EXIT_FAILURE);
	}

	// Harf duyarlılığı olmadan, gelen mesajın hangi transfer modunda iletildiğinin sayısal karşılığının
	mode = strcasecmp(mode_s, "netascii")	?	(strcasecmp(mode_s, "octet") ? 0 : OCTET) :	NETASCII;

	if (mode == 0) {
		printf("%s.%u: Invalid transfer mode (Mail mode is not supported)\n",
				inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
		tftp_server_send_error(socketFilDes, 0, "Invalid transfer mode", client_socket, socketLength);
		exit(EXIT_FAILURE);
	}

	printf("\n%s.%u: CON packet received. Request: \"%s '%s' %s\"\n",			// Server'a istek paketi geldiğini türüyle birlikte kullanıcıya bildirir
			inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port),
			ntohs(message->opcode) == RRQ ? "get" : "put", fileName, mode_s);

	// TODO: NETASCII formatı için handler oluşturulmadı

	if (opcode == RRQ) {	// Server'dan veri okumak için Client'tan istek gelmesi halinde izlenen adımlar
		tftpMessage message;				// tftpMessage bileşimindeki veri türlerinin, fonksiyon içerisinde kullanıma hazır hale getirilmesi

		uint16_t blockNumber = 0;			// Transfer boyunca aktarılan paket sayısının kontrol edileceği değişken
		uint8_t data[TFTP_DATA_DEFAULT];		// Aktarılacak veri boyutunun tanımlanması
		ssize_t dlen;						// Verinin bayt cinsinden boyutunun kaydedileceği değişken
		ssize_t c;						// Gelecek/Gidecek paket boyutunun (bayt) kontrolü

		unsigned int countdownRetry;		// Bir paketin gönderimi için maksimum deneme sayısını belirten değişken
		int to_close = 0;				// İşlem tamamlandığında döngüden çıkmak için kullanılacak değişken

		while (!to_close) {

			dlen = fread(data, 1, sizeof(data), fd); /* Belleğin fp ile işaret edilen adresinden, data'nın toplam (bayt) boyutu adedince veri,
													 1'er bayt'lar halinde, data (tampon belleği) adresine yüklenir. Okunan veri sayısını döndürür.*/
			blockNumber++;		// Verinin blok (paket) numarasının 1 artışı (counter)

			if (dlen < TFTP_DATA_DEFAULT) { 	// Veri boyutu ile gönderilecek son paket olup/olmadığının kontrolü
				to_close = 1;				// TFTP_DATA_DEFAULT'dan az ise son pakettir
			}

			for (countdownRetry = TFTP_RETRY_NUMBER; countdownRetry; countdownRetry--) {	// Paket gönderiminin denenme sayısının sınırlandırması

				c = tftp_send_data(socketFilDes, blockNumber, data, dlen, client_socket, socketLength);	// Server'dan Client'a Data paketi gönderimi
																										// c'de giden paketin boyutu (bayt) saklanır
				if (c < 0) {		// Hata Kontrolü: Paketin gönderilip gönderilemediğinin kontrolü
					printf("%s.%u: transfer killed\n",inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
					exit(EXIT_FAILURE);
				}

				c = tftp_receive_message(socketFilDes, &message, client_socket, &socketLength);	// Client'tan ACK paketi alımı
																								// c'de gelen paketin boyutu (bayt) saklanır
				if (c >= 0 && c < 4) {	// Hata Kontrolü: Gelen paket boyutunun kontrolü
					printf("%s.%u: received packet with invalid size\n", inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
					tftp_server_send_error(socketFilDes, 0, "Invalid request size", client_socket, socketLength);
					exit(EXIT_FAILURE);
				}

				if (c >= 4) {			// Başarılı RRQ paket alımı
					printf("%s.%u: RRQ packet received. (%u)\n", inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port), blockNumber);
					break;
				}

				if (errno != EAGAIN) {	// EAGAIN hatası değilse transferi sonlandır (Resource temporarily unavailable ise gönderim tekrar denenecek)
					printf("%s.%u: transfer killed\n", inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
					exit(EXIT_FAILURE);
				}
			}

			if (!countdownRetry) {		// 5 kez gönderim denenip başarısız olunduğunda transferi sonlandırmak için kullanılır
				printf("%s.%u: transfer timed out\n", inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
				exit(EXIT_FAILURE);
			}

			if (ntohs(message.opcode) == ERROR) {		// Hata Kontrolü: Hata paketi geldiğinde transferi sonlandırır
				printf("%s.%u: error message received: %u %s\n",
						inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port),
			 			ntohs(message.error.errorCode), message.error.errorMessage);
				exit(EXIT_FAILURE);
			}

			if (ntohs(message.opcode) != ACK) {		// Hata Kontrolü: Gelen paketin ACK olup olmadığının kontrolü
				printf("%s.%u: invalid message during transfer received\n",
						inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
				tftp_server_send_error(socketFilDes, 0, "Invalid message during transfer", client_socket, socketLength);
				exit(EXIT_FAILURE);
			}

			if (ntohs(message.ack.blockNumber) != blockNumber) {	// Hata Kontrolü: ACK'nın blok numarası ile
				printf("%s.%u: invalid ack number received\n",		// gönderilen Data'daki blok numarasının uymaması
						inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
				tftp_server_send_error(socketFilDes, 0, "Invalid ack number", client_socket, socketLength);
				exit(EXIT_FAILURE);
			}

		}	// while(!to_close){} bitiş
		printf("\n%s.%u: DONE! Transfer completed with %u sent packet(s).\n",	// Transferin tamamlandığına dair onay mesajı
				inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port), blockNumber);
	} // if(opcode == RRQ) bitiş

	else if (opcode == WRQ) {	// Server'a veri yazmak için Client'tan istek gelmesi halinde izlenen adımlar
		tftpMessage message;			// tftpMessage bileşimindeki veri türlerinin, fonksiyon içerisinde kullanıma hazır hale getirilmesi

		uint16_t blockNumber = 0;		// Transfer boyunca aktarılan paket sayısının kontrol edileceği değişken
		ssize_t c;						// Gelecek/Gidecek paket boyutunun (bayt) kontrolü

		unsigned int countdownRetry;	// Bir paketin gönderimi için maksimum deneme sayısını belirten değişken
		int to_close = 0;				// İşlem tamamlandığında döngüden çıkmak için kullanılacak değişken

		c = tftp_send_ack(socketFilDes, blockNumber, client_socket, socketLength);	// Client'ın isteğine karşılık ACK paketi gönderimi

		if (c < 0) {		// Hata Kontrolü: ACK paketinin, gönderilip gönderilemediğinin kontrolü
			printf("%s.%u: transfer killed\n", inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
			exit(EXIT_FAILURE);
		}

		while (!to_close) {

			for (countdownRetry = TFTP_RETRY_NUMBER; countdownRetry; countdownRetry--) {	// Paket gönderiminin denenme sayısının sınırlandırması

				c = tftp_receive_message(socketFilDes, &message, client_socket, &socketLength); // Server'dan Client'a Data paketi gönderimi

				if (c >= 0 && c < 4) { // Hata Kontrolü: Gelen paket boyutunun kontrolü
					printf("%s.%u: message with invalid size received\n", inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
					tftp_server_send_error(socketFilDes, 0, "Invalid request size", client_socket, socketLength);
					exit(EXIT_FAILURE);
				}

				if (c >= 4) {	// Başarılı WRQ paket alımı
					break;
				}

				if (errno != EAGAIN) {	// EAGAIN hatası değilse transferi sonlandır (Resource temporarily unavailable ise gönderim tekrar denenecek)
					printf("%s.%u: transfer killed\n", inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
					exit(EXIT_FAILURE);
				}

				c = tftp_send_ack(socketFilDes, blockNumber, client_socket, socketLength);	// Server'dan Client'a ACK paketi gönderimi

				if (c < 0) {		// Hata Kontrolü: Paketin gönderilip gönderilemediğinin kontrolü
					printf("%s.%u: transfer killed\n", inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
					exit(EXIT_FAILURE);
				}
			}

			if (!countdownRetry) {	// 5 kez gönderim denenip başarısız olunduğunda transferi sonlandırmak için kullanılır
				printf("%s.%u: transfer timed out\n", inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
				exit(EXIT_FAILURE);
			}

			blockNumber++;	// Verinin blok (paket) numarasının 1 artışı (counter)
			printf("%s.%u: WRQ packet received. (%u)\n", inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port), blockNumber);

			if (c < sizeof(message.data)) {
				to_close = 1;
			}

			if (ntohs(message.opcode) == ERROR) {	// Hata Kontrolü: Hata paketi geldiğinde transferi sonlandırır
				printf("%s.%u: error message received: %u %s\n",
						inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port),
						ntohs(message.error.errorCode), message.error.errorMessage);
				exit(EXIT_FAILURE);
			}

			if (ntohs(message.opcode) != DATA) {	// Hata Kontrolü: Gelen paketin Data türünde olup olmadığının kontrolü
				printf("%s.%u: invalid message during transfer received\n",
						inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
				tftp_server_send_error(socketFilDes, 0, "Invalid message during transfer", client_socket, socketLength);
				exit(EXIT_FAILURE);
			}

			if (ntohs(message.ack.blockNumber) != blockNumber) {	// Hata Kontrolü: Gelen Data'daki blok numarası ile
				printf("%s.%u: invalid block number received\n", 	// gönderilen ACK'nın blok numarasının uymaması
						inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
				tftp_server_send_error(socketFilDes, 0, "Invalid block number", client_socket, socketLength);
				exit(EXIT_FAILURE);
			}

			c = fwrite(message.data.data, 1, c - 4, fd);	/* Belleğin data (tampon belleği) adresinden, 1'er bayt boyutunda, gelen Data paketinin
								(bayt cinsinden) boyutunun 4 eksiği adedince veri, fp ile işaret edilen adrese yüklenir. Yazılan veri sayısını döndürür.*/
			if (c < 0) {	// Hata Kontrolü: Gelen verinin yazılmasında hata
				perror("server: fwrite()");
				exit(EXIT_FAILURE);
			}

			c = tftp_send_ack(socketFilDes, blockNumber, client_socket, socketLength);	// Data paketinin alındığına dair Client'a ACK paketi gönderimi

			if (c < 0) {	// Hata Kontrolü: ACK paketinin, gönderilip gönderilemediğinin kontrolü
				printf("%s.%u: transfer killed\n", inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port));
				exit(EXIT_FAILURE);
			}
		} // while(!to_close) bitiş
		printf("\n%s.%u: DONE! Transfer completed with %u received packet(s).\n",	// Verinin başarıyla alındığına dair çıktı
				inet_ntoa(client_socket->sin_addr), ntohs(client_socket->sin_port), blockNumber);
	} // else if (opcode == WRQ) bitiş

	fclose(fd);			// fd FILE nesnesi işaretçisiyle gösterilen dosya akışı kapatılır ve tampon bellekler temizlenir.
	close(socketFilDes);	// Soketin sonlandırılması

	exit(EXIT_SUCCESS);
}

// Client'tan Gelecek İsteklerin Beklenip Yönlendirileceği Fonksiyon
int tftp_server_listen(int socketFilDes){

	while (1) {
		struct sockaddr_in client_sock;		// Client bilgilerinin yer alacağı struct'ın oluşturulması
		socklen_t slen = sizeof(client_sock);	// Client soket struct'ının toplam kapladığı alanın unsigned int cinsinden kaydedilmesi
		ssize_t receivedMesSize;				// Gelen mesaj uzunluğunun yer alacağı değişkenin tanımlanması (mesajın kaç bayt olduğu)

		tftpMessage message;					// tftpMessage bileşimindeki veri türlerinin, fonksiyon içerisinde kullanıma hazır hale getirilmesi
		uint16_t opcode;						// unsigned short int türünden işlem kodu değişkeni tanımlanması

		if ((receivedMesSize = tftp_receive_message(socketFilDes, &message, &client_sock, &slen)) < 0) {	// Gelen mesaj kontrolü. Cevap yoksa
			continue;														// diğer işlemleri atlayıp döngünün başlangıcına dönülür
		}

		if (receivedMesSize < TFTP_DATA_MINIMUM) {		// Hata Kontrolü: Gelen mesaj boyutunun minimum kabul edilen değere göre durumu
			printf("%s.%u: received packet with invalid size\n",
					inet_ntoa(client_sock.sin_addr), ntohs(client_sock.sin_port));	// Client ip&port bilgileri ile hata mesajı bastırma
			tftp_server_send_error(socketFilDes, 0, "Invalid request size", &client_sock, slen);
			continue;
		}

		opcode = ntohs(message.opcode);

		if (opcode == RRQ || opcode == WRQ) {	// Gelen mesajın okuma/yazma isteği paketi olup olmadığının kontrolü
			// Gelen paket içeriği, oluşturulan child process'te okunur. Bu sırada parent process else'te child process'in sonlandırılmasını bekler
			if (fork() == 0) {		// Child process'in girip işlem yaptığı blok
				tftp_server_handle_request(&message, receivedMesSize, &client_sock, slen);	// Gelen mesajın işlenme adımı
				exit(EXIT_SUCCESS);	// Parent process sonlandırılır ve child process, yeni parent process olarak çalışmaya devam eder
			}
			else {		// Parent process'in girip child process'in sonlandırılmasını bekleyeceği blok
				wait(NULL);
			}
		}

		else {		// Gelen paketin hatalı olma durumunda izlenecek adımlar
			printf("Invalid request received from %s.%u | opcode: \"%u\" \n",
					inet_ntoa(client_sock.sin_addr), ntohs(client_sock.sin_port), opcode);
			tftp_server_send_error(socketFilDes, 0, "Invalid opcode", &client_sock, slen);
		}
	} // while(1){} bitiş

	return socketFilDes;
}

void tftp_ip_lister(){

	#include <ifaddrs.h>
	struct ifaddrs *ifap, *ifa;
	struct sockaddr_in *sa;
	char *addr;

	getifaddrs (&ifap);
	for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr && ifa->ifa_addr->sa_family==AF_INET && strcmp(ifa->ifa_name, "lo")) {
			sa = (struct sockaddr_in *) ifa->ifa_addr;
			addr = inet_ntoa(sa->sin_addr);
			printf("Listening address %s\n", addr);
		}
	}

	freeifaddrs(ifap);
}

// Main Fonksiyonu
int main(int argc, char *argv[])
{
	int socketFilDes;				// Client dinlemede kullanılan soket (Socket File Descriptor)
	uint16_t port = 0;				// Kullanılacak port numarasının değişken tanımlaması
	struct protoent *pp;			// Protokol veritabanı bilgilerinin yer alacağı struct'ın oluşturulması
	struct servent *ss;				// Servis veritabanı bilgilerinin yer alacağı struct'ın oluşturulması
	struct sockaddr_in server_sock;	// Server Soket bilgilerinin yer alacağı struct'ın oluşturulması

	if (argc < 2 || argc > 3) {	// Hata Kontrolü: Program başlangıcında girilen argüman sayısının kontrolü
		printf("Usage:\n\t%s [base directory] [port number]\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	fileBaseDirectory = argv[1];	// Server'ın kullanacağı ana dizinin, kullanıcıdan gelen argümanla tanımlanması

	if (chdir(fileBaseDirectory) < 0) {	// Hata Kontrolü: Geçerli dizinin, girilen dizinle değiştirilip geçerliliğinin kontrolü
		perror("Server: chdir()");
		exit(EXIT_FAILURE);
	}

	if (argc > 2) {	// Programa başlangıçta ana dizin ve port numarasının girilmesi halinde yapılacak eylemler
		if (sscanf(argv[2], "%hu", &port)) { // Hata Kontrolü: Programa başlangıçta girilen port no'nun kaydı | Fonk. parametre sayısını döndürür
			port = htons(port);	// Port numarası verisinin ağ bayt sıralamasına çevrilip kaydedilmesi
		} else {	// Hata kontrolü: Programa girilen port numarası argümanının hatalı olması durumu
			fprintf(stderr, "Server: invalid port number\n");
			exit(EXIT_FAILURE);
		}
	} else {	// Programa yalnızca çalışma dizininin verilmesi halinde yapılacaklar (port girilmediğinde)
		if ((ss = getservbyname("tftp", "udp")) == 0) {			// Hata Kontrolü: Mevcut protokol ve hizmet adına uygun
			fprintf(stderr, "Server: getservbyname() error\n");	//			   port numarasının kontrolü ve kaydedilmesi
			exit(EXIT_FAILURE);
		}
	}

	if ((pp = getprotobyname("udp")) == 0) {				// Hata Kontrolü: UDP protokolüne ait protokol numarasının
		fprintf(stderr, "Server: getprotobyname() error\n");	// 			   varlığının kontrolü ve kaydedilmesi
		exit(EXIT_FAILURE);
	}

	if ((socketFilDes = socket(AF_INET, SOCK_DGRAM, pp->p_proto)) == -1) {	// Hata Kontrolü: Soket oluşturulması ve hata durumunun kontrolü
		perror("Server: socket() error");
		exit(EXIT_FAILURE);
	}

	// Server ağ ayarlarının tanımlanması
	server_sock.sin_family = AF_INET;				// Soket için kullanılan iletişim protokolü ailesi (IPv4)
	server_sock.sin_addr.s_addr = htonl(INADDR_ANY);	// Soketin tüm yerel arabirimlere bağlanacağının belirtilmesi
	server_sock.sin_port = port ? port : ss->s_port;	// Kullanılacak port numarasının atanması

	// Hata Kontrolü: Soketin belirlenen ip adresi ve port numarası ile ilişkilendirilip hata kontrolünün yapılması
	// bind() fonksiyonu hata halinde "-1" değerini döndürmektedir.
	if (bind(socketFilDes, (struct sockaddr *) &server_sock, sizeof(server_sock)) == -1) {
		perror("Server: bind()");
		puts("Hint: You can try with sudo or a different port number.");
		close(socketFilDes);		// Soketin sonlandırılması
		exit(EXIT_FAILURE);
	}

	printf("\nTFTP Server: listening on %d\n", ntohs(server_sock.sin_port));

	tftp_ip_lister();

	puts("You can exit the program with Ctrl+C.");

	socketFilDes = tftp_server_listen(socketFilDes);

	close(socketFilDes);	// Soketin sonlandırılması

	return 0;
}