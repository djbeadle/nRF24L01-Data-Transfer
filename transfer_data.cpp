// For controlling the radio module
#include <RF24/RF24.h>

// For cout and so forth
#include <iostream>

// For signal handling
#include <csignal>

// For getopt
#include <unistd.h>

// For operating on files
#include <fstream>

// For the timestamps
#include <ctime>
#include <chrono>

// For reading from the accelerometer using the wiring pi drivers
#include <wiringPi.h>
#include <ads1115.h>

using namespace std;

/********************************
 * User Configurable Variables: *
********************************/

// Sampling rate
const uint8_t measure_seconds = 4;

/****************
 * Radio Config *
*****************/

// Radio CE Pin, CSN Pin, SPI Speed
// See http://www.airspayce.com/mikem/bcm2835/group__constants.html#ga63c029bd6500167152db4e57736d0939 and the related enumerations for pin information.

/* When using the "rpi" driver (The default driver on a Raspberry Pi) */
// Setup for GPIO 22 CE and CE0 CSN with SPI Speed @ 4Mhz
// RF24 radio(RPI_V2_GPIO_P1_22, BCM2835_SPI_CS0, BCM2835_SPI_SPEED_4MHZ);

/* When using the "wiringPi" driver */
// Setup for GPIO 22 CE and CE0 CSN with SPI Speed @ 4Mhz
// RF24 radio(6, 10, BCM2835_SPI_SPEED_4MHZ)

RF24 radio(6,11);

/*********************
 * System Variables: *
**********************/

// Radio pipe addresses for the 2 nodes to communicate.
const uint64_t addresses[2] = { 0xABCDABCD71LL, 0x544d52687CLL };

static volatile int interrupt_flag = 0;	// Catches Ctrl-c, for canceling transmisison
static volatile int alarm_sounded = 0; // For measuring transmission rate

// For determining how many packets we've received in this time interval when measuring reception rate
volatile uint16_t num_recvd_last = 0;

int hide = 1;

// Deal with user interrupts
void interrupt_handler(int nothing)
{
	cin.clear();
	cout << "Ctrl-c pressed! Ending transmission and truncuating file.\n";
	interrupt_flag = 1;
}

// What second is it?
void sigalrm_handler(int sig)
{
	alarm_sounded = true;
}

/*
 * For the packet checksums
 */
uint8_t fletcher_8(uint8_t *data, size_t size)
{
	uint8_t sum1 = 0;
	uint8_t sum2 = 0;
	//printf("fletcher computation: \n");
	while(size--)
	{
	//	printf("i: %d, c: %c\n", size, *data);
		sum1+=*data++;
		sum2+= sum1;
	}
	return (sum1&0xF) | (sum2<<4);
}

int send_packet(uint16_t second, uint16_t ctr, int16_t x, int16_t y, int16_t z)
{
	char code[32];
	memset(&code, '\0', 32);

	memcpy(code, &second, 2);
	memcpy(code+2, &ctr, 2);

	memcpy(code+4, &x, 2);
	memcpy(code+6, &y, 2);
	memcpy(code+8, &z, 2);

	uint8_t chk_sum = fletcher_8((uint8_t*)&code[0], 10);

	cout<< "Packet looks like this:\n";
	/* 
	printf("second:	%u\n", code);
	printf("ctr:	%u\n", code+2);
	printf("x:		%d\n", code+4);
	printf("y:		%d\n", code+6);
	printf("z:		%d\n", code+8);
	*/
	printf("second:	%u\n", second);
	printf("ctr:	%u\n", ctr);
	printf("x:	%d\n", x);
	printf("y:	%d\n", y);
	printf("z:	%d\n", z);

	return 1;
}

/*
 * TODO:
 *  - Measure transfer rate
 */
int main(int argc, char** argv)
{
	signal(SIGINT, interrupt_handler); // Ctrl-c interrupt handler
	fstream *file; // file to read from or write to
    char *filename = NULL; 
	uint8_t *packets; // buffer to store all of the packets in before writing to file

	const bool role_tx = 1, role_rx = 0;
	bool role = 0;

	bool measure = false;
	bool hide_progress_bar = false;

	bool z = false; // Flag to make sure we don't set both the -s and -d flags
	int c;
    while ((c = getopt (argc, argv, "s:d:nmhD")) != -1)
	{
		switch (c)
		{
			case 'D':
				hide = 0;
				break;
			case 'h':
				cout << "This is a wireless data transfer utility built for the nRF24 radio family!\n";
				cout << "It's built using TMRh20's C++ RF24 library, which can be found on Github:\n";
				cout << "https://github.com/nRF24/RF24";
				cout << "\n";
				cout << "Usage:\n";
				cout << "-h: Show this help text.\n";
				cout << "-s: Should be followed by the source file. Use this on the transmitter.\n";
				cout << "-d: Should be followed by the destination file. Use this on the receiver. It will overwrite any existing files.\n";
				cout << "-D: Show a bunch of debug messages. \n";
				cout << "-m: Measure the successfull data reception rate. Doesn't count packets where checksums don't match\n";
				cout << "\n";
				cout << "Examples:\n";
				cout << "sudo ./data_transfer -s ModernMajorGeneral.txt \n";
				cout << "sudo ./data_transfer -d ModernMajorGeneral-recv.txt \n";
				break;	
			case 's': // Specify source file
				if(z == true)
				{
					cout << "Cannot be both transmitter and receiver!\n";
					return 25; 
				}
				filename = optarg;
				z = true;
				role = role_tx;
				break;
			case 'd': // Specify destination file
				if(z == true)
				{
					cout << "Cannot be both transmitter and receiver!\n";
					return 25;
				}
				filename = optarg;
				z = true;
				role = role_rx;
				break;
			case 'm': // Measure data reception rate
				measure = true;
				cout << "Measuring!\n";
				break;
			case 'n': // Hide the progress bar
				hide_progress_bar = true;
				cout << "Hiding progress bar!\n";
				break;
			case '?':
				fprintf (stderr, "Unknown option `-%c'.\n", optopt);
				return 6;
		} // switch
    } // getopts while loop

    if (measure == true && role == role_tx)
	{
		cout << "ERROR: Cannot measure data reception rate from the transmitter.\n";
		return 6;
	}
	
	// Make sure the user specified a file. 
	if(z != true)
	{
		cout << "ERROR: At least one filename is required as an agrument. Use -s [source file] or -d [dest file]\n";
		return 6;
	}

	// Open the file
	file = new fstream(filename, fstream::in);
	if(file == NULL)
	{
		cout << "Could not open the file.\n";
		return 6;
	}

    /*******************************/
	/* PRINT PREAMBLE AND GET ROLE */
	/*******************************/
	if(hide == 0)
		cout << "RF24/examples/combined2.cpp\n";

	radio.begin();                           // Setup and configure rf radio
	radio.flush_tx();
	radio.flush_rx();
	radio.setChannel(110); 			// Channel choice can have a big effect on packet corruption. 
	radio.setPALevel(RF24_PA_MAX);
	radio.setDataRate(RF24_2MBPS);
	radio.setAutoAck(1);                     // Ensure autoACK is enabled
	radio.setRetries(4,15);                  // Optionally, increase the delay between retries & # of retries
	radio.setCRCLength(RF24_CRC_16);          // Use 8-bit CRC for performance

	if(hide == 0){
		radio.printDetails();
	}

    /************/
	/* RECEIVER */
	/************/
    // if(role == role_rx)
	// {
	// 	if(measure == true)
	// 	{
	// 		signal(SIGALRM, &sigalrm_handler);
	// 	}

    //     /* Open a file for writing the output to */
	// 	FILE *output_file;
    //     output_file = fopen(filename, "w");

	// 	if(output_file == NULL)
	// 	{
	// 		cout << "Something weird happened trying to write to the file\n";
	// 		perror("The following error occurred: ");
	// 		return 6;
	// 	}

    //     // Initialize the radio
    //     radio.openWritingPipe(addresses[0]);
	// 	radio.openReadingPipe(1,addresses[1]);
    //     radio.startListening();

	// 	/* 
	// 	 * Control flag:
	// 	 * 0 - have not received starting packet
	// 	 * 1 - starting packet received, ready for data pkts
	// 	 */
	// 	int control = 0;

    //     // Check if the user has canceled the data transfer before doing all this fun stuff
	// 	if(interrupt_flag != 0)
	// 	{
	// 		cout << "File transfer canceled by user.\n";
	// 		return 6;
	// 	}
        
    //     /* Packet RX Loop: */
	// 	uint8_t data[32]; // Holds the current packet while we process it
	// 	cout << "Waiting for transmission...\n";
    //     while(interrupt_flag ==0 )
    //     {
    //         // How many packets have we received in this interval?
    //         if(measure == true && timer_flag == true)
    //         {
    //             unsigned long recvd_this_interval = num_recvd - num_recvd_last;
	// 			unsigned long rate_this_interval = recvd_this_interval / measure_seconds;
	// 			int data_rate = rate_this_interval * num_payload_bytes;
	// 			printf("Received %u pkts in %u seconds - %u pkts/sec	- %d bytes/sec \n", recvd_this_interval, measure_seconds, recvd_this_interval / measure_seconds, data_rate);

	// 			num_recvd_last = num_recvd;
	// 			timer_flag = false;
	// 			alarm(measure_seconds);
    //         }
            
    //         // Process data packets
    //         if(radio.available())
    //         {
    //             radio.read(&data, 32);
    //             uint16_t 

    //         } // process a data packet

    //     } // packet rx loop
    // } // receiver
	

    /***************/
	/* TRANSMITTER */
	/***************/
	else if(role == role_tx)
	{
        // Initialize the radio
        radio.openWritingPipe(addresses[1]);
		radio.openReadingPipe(1,addresses[0]);
		radio.stopListening();

		// Initialize the accelerometer
		ads1115Setup(100, 0x48);
		// Set the sample rate. I think this is the fastest
		digitalWrite(101,6);

		// Variables for the axis values:
		int16_t x, y, z;

		// What second is it?
		uint16_t second = 0;

		int sampling_time = 5; // seconds
		// alarm(1); // Alarm goes off every second
		uint16_t ctr = 0; // Number of packets we've sent each second

		while (sampling_time > 0)
		{
			/* if(alarm_sounded == 1)
			{
				ctr = 0;
				second++;
			}*/ 
			y = (int16_t)analogRead(101);
			x = (int16_t)analogRead(100);
			z = (int16_t)analogRead(102);

			send_packet(second, ctr, x, y, z);
			ctr++;
		}
		

    } // transmitter


} // main 

// Initalize the connection to the receiver
/* int tx_initialize_connection()
{
    uint8_t first[32] = memset(&first, '\0', sizeof(first));
    first[1] = '1';

    cout << "Attempting to establish connection...";
    cout.flush();
    while(interrupt_flag == 0)
    {
        if(radio.write(first, sizeof(first)) == false)
        {
            if(hide!=1) cout << "Sending first packet failed.\n";
        }
        else break;
    }
    if(interrupt_flag != 0)
    {
        cout << "Attempt to establish a connection was canceled by the user.\n";
        return 6;
    }
    cout << "Success!\n";
    return 0;

}*/

// Initalize the connection to the transmitter
int rx_initialize_connection()
{
    uint8_t data[32];
    /* Loop until first packet is received or user cancels it with ctrl-c */
    while (interrupt_flag == 0)
    {
        if(radio.available())
        {
            radio.read(&data, 32);
            if((char*)data[0] == '\0' && (char)data[1] == '1'){
                std::cout << "\n";
                std::cout << "Data transfer beginning!\n";
                return 0;
            }
        }
    }
    printf("Data transfer canceled by the user!\n");
    return 1;
}