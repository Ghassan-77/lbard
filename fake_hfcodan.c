#include "fakecsmaradio.h"


int hfcodan_read_byte(int i,unsigned char c)
{
  int versionHi=4, versionLo=2;
  
  if (c==0x15) {
    // Control-U -- clear input buffer
    clients[i].buffer_count=0;
  } else if (c!='\n'&&c!='\r'&&c) {
    // fprintf(stderr,"Radio #%d received character 0x%02x\n",i,c);    
    if (clients[i].buffer_count<(CLIENT_BUFFER_SIZE-1))
      clients[i].buffer[clients[i].buffer_count++]=c;
  } else {
    if (clients[i].buffer_count) {
      clients[i].buffer[clients[i].buffer_count]=0;
      fprintf(stderr,"Radio #%d sent command '%s'\n",i,clients[i].buffer);
      
      // Process the command here
      if (!strcmp((const char *)clients[i].buffer,"VER"))
	{
	  char buffer2[8192];
	  fprintf(stderr,"its VER \n");
	  memset(buffer2, 0, sizeof buffer2);
	  snprintf(buffer2,sizeof buffer2,"VER\r\nCICS: V%d.%d\r\n",versionHi,versionLo);
	  write(clients[i].socket, buffer2, sizeof buffer2);
	}

      // Reset buffer ready for next command
      clients[i].buffer_count=0;
    }    
  }

  return 0;
}
