#include <strings.h>

#include "fakecsmaradio.h"

// Commands and responses we implement
// "AXLINK'+<link partner> make connection to peer
//     (modem doesn't respond preemptively, must be queried with AILTBL)
// "AILTBL" - query current ALE link status
//   "AILTBL" - ALE not connected/no longer connected
//   "AILTBL"+<linkpartner> - ALE link established to this partner
// "AXNMGS"+<linkpartner>+<two digit message length in decimal>+<message text>
//    OK or EV response after sending
//    "AIAMDM"+<message> - ALE message received

unsigned char barrett_e0_string[6]={0x13,'E','0',13,10,0x11};

int hfbarrett_read_byte(int i,unsigned char c)
{
  if (c==0x15) {
    // Control-U -- clear input buffer
    clients[i].buffer_count=0;
  } else if ((c!='\n')&&(c!='\r')&&c) {
    // fprintf(stderr,"Radio #%d received character 0x%02x\n",i,c);

    // No echo by default for Barrett radios
    // write(clients[i].socket,&c,1);
    
    if (clients[i].buffer_count<(CLIENT_BUFFER_SIZE-1))
      clients[i].buffer[clients[i].buffer_count++]=c;
  } else {
    if (clients[i].buffer_count) {

      // No CRLF echo by default for Barrett radios
      // write(clients[i].socket,"\r\n",2);
      
      clients[i].buffer[clients[i].buffer_count]=0;
      fprintf(stderr,"Barrett HF Radio #%d sent command '%s'\n",i,clients[i].buffer);

      // Process the command here
      if (!strncasecmp("AXNMSG",(char *)clients[i].buffer,6)) {
	// Send ALE message
	// XXX- implement me!
	fprintf(stderr,"Saw AXNMSG command\n");
      } else {
	// Complain about unknown commands
	fprintf(stderr,"Responding with Barrett E0 string\n");
	write(clients[i].socket,barrett_e0_string,6);
      }

      // Reset buffer ready for next command
      clients[i].buffer_count=0;
    }    
  }

  return 0;
}

int hfbarrett_heartbeat(int client)
{
  return 0;
}

int hfbarrett_encapsulate_packet(int from,int to,
				 unsigned char *packet,
				 int *packet_len)
{
  return 0;
}
