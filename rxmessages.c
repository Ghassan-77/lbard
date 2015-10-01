/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2015 Serval Project Inc.

This program monitors a local Rhizome database and attempts
to synchronise it over low-bandwidth declarative transports, 
such as bluetooth name or wifi-direct service information
messages.  It is intended to give a high priority to MeshMS
converations among nearby nodes.

The design is fully asynchronous, so a call to the update_my_message()
function from time to time should be all that is required.


This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <assert.h>

#include "lbard.h"

extern char *my_sid_hex;

int saw_length(char *peer_prefix,char *bid_prefix,long long version,
	       int body_length)
{
  return 0;
}

int saw_piece(char *peer_prefix,char *bid_prefix,long long version,
	      long long piece_offset,int piece_bytes,int is_end_piece,
	      int is_manifest_piece,unsigned char *piece,

	      char *prefix, char *servald_server, char *credential)
{
  int peer=find_peer_by_prefix(peer_prefix);
  if (peer<0) return -1;

  if (debug_pieces) fprintf(stderr,"Saw a bundle piece from SID=%s*\n",peer_prefix);

  int bundle_number=-1;

  // Schedule BAR for announcement immediately if we already have this version of this
  // bundle, so that the sender knows that they can start sending something else.
  // This in effect provides a positive ACK for reception of a new bundle.
  // XXX - If the sender depends on the ACK to start sending the next bundle, then
  // an adversary could purposely refuse to acknowledge bundles (that it might have
  // introduced for this special purpose) addressed to itself, so that the priority
  // scheme gets stuck trying to send these bundles to them forever.
  for(int i=0;i<bundle_count;i++) {
    if (!strncasecmp(bid_prefix,bundles[i].bid,strlen(bid_prefix))) {
      if (debug_pieces) fprintf(stderr,"We have version %lld of BID=%s*.  %s is offering us version %lld\n",
	      bundles[i].version,bid_prefix,peer_prefix,version);
      if (version<=bundles[i].version) {
	// We have this version already: mark it for announcement to sender,
	// and then return immediately.
	bundles[i].announce_bar_now=1;
	if (debug_pieces) fprintf(stderr,"We already have %s* version %lld - ignoring piece.\n",
		bid_prefix,version);
	return 0;
      } else {
	// We have an older version.
	// Remember the bundle number so that we can pre-fetch the body we have
	// for incremental journal transfers
	if (version<0x100000000LL) {
	  bundle_number=i;
	}
	
      }
    }
  }

  int i;
  int spare_record=random()%MAX_BUNDLES_IN_FLIGHT;
  for(i=0;i<MAX_BUNDLES_IN_FLIGHT;i++) {
    if (!peer_records[peer]->partials[i].bid_prefix) {
      if (spare_record==-1) spare_record=i;
    } else {
      if (!strcasecmp(peer_records[peer]->partials[i].bid_prefix,bid_prefix))
	{
	  if (debug_pieces) fprintf(stderr,"Saw another piece for BID=%s* from SID=%s: ",
			 bid_prefix,peer_prefix);
	  if (debug_pieces) fprintf(stderr,"[%lld..%lld)\n",
			 piece_offset,piece_offset+piece_bytes);

	  break;
	}
    }
  }

  if (1) fprintf(stderr,"Saw a piece of interesting bundle BID=%s*/%lld from SID=%s\n",
		 bid_prefix,version, peer_prefix);

  if (i==MAX_BUNDLES_IN_FLIGHT) {
    // Didn't find bundle in the progress list.
    // Abort one of the ones in the list at random, and replace, unless there is
    // a spare record slot to use.
    if (spare_record==-1) {
      i=random()%MAX_BUNDLES_IN_FLIGHT;
      clear_partial(&peer_records[peer]->partials[i]);
    } else i=spare_record;

    // Now prepare the partial record
    peer_records[peer]->partials[i].bid_prefix=strdup(bid_prefix);
    peer_records[peer]->partials[i].bundle_version=version;
    peer_records[peer]->partials[i].manifest_length=-1;
    peer_records[peer]->partials[i].body_length=-1;
  }

  int piece_end=piece_offset+piece_bytes;

  // Note stream length if this is an end piece or journal bundle
  if (is_end_piece) {
    if (is_manifest_piece)
      peer_records[peer]->partials[i].manifest_length=piece_end;
    else
      peer_records[peer]->partials[i].body_length=piece_end;
  }
  if (version<0x100000000LL) {
    // Journal bundle, so body_length = version
    peer_records[peer]->partials[i].body_length=version;
  }

  if ((bundle_number>-1)
      &&(!peer_records[peer]->partials[i].body_segments)) {
    // This is a bundle that for which we already have a previous version, and
    // for which we as yet have no body segments.  So fetch from Rhizome the content
    // that we do have, and prepopulate the body segment.
    if (!prime_bundle_cache(bundle_number,my_sid_hex,servald_server,credential)) {
      struct segment_list *s=calloc(1,sizeof(struct segment_list));
      assert(s);
      s->data=malloc(cached_body_len);
      assert(s->data);
      bcopy(cached_body,s->data,cached_body_len);
      s->start_offset=0;
      s->length=cached_body_len;
      peer_records[peer]->partials[i].body_segments=s;
      fprintf(stderr,"Preloaded %d bytes from old version of journal bundle.\n",
	      cached_body_len);
    } else {
      fprintf(stderr,"Failed to preload bytes from old version of journal bundle. XFER will likely fail due to far end thinking it can skip the bytes we already have, so ignoring current piece.\n");
      return -1;
    }
  }

  // Now we have the right partial, we need to look for the right segment to add this
  // piece to, if any.
  struct segment_list **s;
  if (is_manifest_piece) s=&peer_records[peer]->partials[i].manifest_segments;
  else s=&peer_records[peer]->partials[i].body_segments;

  /*
    The segment lists are maintained in reverse order, since pieces will generally
    arrive in ascending address order.
  */
  int segment_start;
  int segment_end;
  while(1) {
    if (*s) {
      segment_start=(*s)->start_offset;
      segment_end=segment_start+(*s)->length;
    } else {
      segment_start=-1; segment_end=-1;
    }
    
    if ((!(*s))||(segment_end<piece_offset)) {
      // Create a new segment before the current one

      if (debug_pieces) fprintf(stderr,"Inserting piece [%lld..%lld) before [%d..%d)\n",
		     piece_offset,piece_offset+piece_bytes,
		     segment_start,segment_end);

      struct segment_list *ns=calloc(1,sizeof(struct segment_list));
      assert(ns);

      // Link into the list
      ns->next=*s;
      if (*s) ns->prev=(*s)->prev; else ns->prev=NULL;
      if (*s) (*s)->prev=ns;
      *s=ns;

      // Set start and ends and allocate and copy in piece data
      ns->start_offset=piece_offset;
      ns->length=piece_bytes;
      ns->data=malloc(piece_bytes);
      bcopy(piece,ns->data,piece_bytes);

      break;
    } else if ((segment_start<=piece_offset)&&(segment_end>=piece_end)) {
      // Piece fits entirely within a current segment, i.e., is not new data
      break;
    } else if (piece_end<segment_start) {
      // Piece ends before this segment starts, so proceed down the list further.
      fprintf(stderr,"Piece [%lld..%lld) comes before [%d..%d)\n",
	      piece_offset,piece_offset+piece_bytes,
	      segment_start,segment_end);

      s=&(*s)->next;
    } else {
      // Segment should abutt or overlap with new piece.
      // Pieces can be different sizes, so it is possible to extend both directions
      // at once.

      // New piece and existing segment should overlap or adjoin.  Otherwise abort.
      int piece_start=piece_offset;
      assert( ((segment_start>=piece_start)&&(segment_start<=piece_end))
	      ||((segment_end>=piece_start)&&(segment_end<=piece_end))
	      );      

      fprintf(stderr,"Received %s",bid_prefix);
      fprintf(stderr,"* version %lld %s segment [%d,%d)\n",
	      version,
	      is_manifest_piece?"manifest":"payload",
	      piece_start,piece_start+piece_bytes);
            
      if (piece_start<segment_start) {
	// Need to stick bytes on the start
	int extra_bytes=segment_start-piece_start;
	int new_length=(*s)->length+extra_bytes;
	unsigned char *d=malloc(new_length);
        assert(d);
	bcopy(piece,d,extra_bytes);
	bcopy((*s)->data,&d[extra_bytes],(*s)->length);
	(*s)->start_offset=piece_start;
	(*s)->length=new_length;
	free((*s)->data); (*s)->data=d;
      }
      if (piece_end>segment_end) {
	// Need to sick bytes on the end
	int extra_bytes=piece_end-segment_end;
	int new_length=(*s)->length+extra_bytes;
	(*s)->data=realloc((*s)->data,new_length);
        assert((*s)->data);
	bcopy(&piece[piece_bytes-extra_bytes],&(*s)->data[(*s)->length],
	      extra_bytes);
	(*s)->length=new_length;
      }
      
      break;
    } 
  }

  merge_segments(&peer_records[peer]->partials[i].manifest_segments);
  merge_segments(&peer_records[peer]->partials[i].body_segments);

  // Check if we have the whole bundle now
  if (peer_records[peer]->partials[i].manifest_segments
      &&peer_records[peer]->partials[i].body_segments
      &&(!peer_records[peer]->partials[i].manifest_segments->next)
      &&(!peer_records[peer]->partials[i].body_segments->next)
      &&(peer_records[peer]->partials[i].manifest_segments->start_offset==0)
      &&(peer_records[peer]->partials[i].body_segments->start_offset==0)
      &&(peer_records[peer]->partials[i].manifest_segments->length
	 ==peer_records[peer]->partials[i].manifest_length)
      &&(peer_records[peer]->partials[i].body_segments->length
	 ==peer_records[peer]->partials[i].body_length))
    {
      // We have a single segment for body and manifest that span the complete
      // size.
      fprintf(stderr,">>> We have the entire bundle now.\n");
      rhizome_update_bundle(peer_records[peer]->partials[i].manifest_segments->data,
			    peer_records[peer]->partials[i].manifest_length,
			    peer_records[peer]->partials[i].body_segments->data,
			    peer_records[peer]->partials[i].body_length,
			    servald_server,credential);
      // Now release this partial.
      clear_partial(&peer_records[peer]->partials[i]);
    }

  return 0;
}


int saw_message(unsigned char *msg,int len,char *my_sid,
		char *prefix, char *servald_server,char *credential)
{
  /*
    Parse message and act on it.    
  */
  
  // All valid messages must be at least 8 bytes long.
  if (len<8) return -1;
  char peer_prefix[6*2+1];
  snprintf(peer_prefix,6*2+1,"%02x%02x%02x%02x%02x%02x",
	   msg[0],msg[1],msg[2],msg[3],msg[4],msg[5]);
  int msg_number=msg[6]+256*(msg[7]&0x7f);
  int is_retransmission=msg[7]&0x80;

  // Ignore messages from ourselves
  if (!bcmp(msg,my_sid,6)) return -1;
  
  if (debug_pieces) {
    fprintf(stderr,"Decoding message #%d from %s*, length = %d:\n",
	    msg_number,peer_prefix,len);
  }

  int offset=8; 

  char bid_prefix[8*2+1];
  long long version;
  int size_byte;
  char recipient_prefix[4*2+1];
  unsigned int offset_compound;
  long long piece_offset;
  int piece_bytes;
  int piece_is_manifest;
  int above_1mb;
  int is_end_piece;

  // Find or create peer structure for this.
  struct peer_state *p=NULL;
  for(int i=0;i<peer_count;i++) {
    if (!strcasecmp(peer_records[i]->sid_prefix,peer_prefix)) {
      p=peer_records[i]; break;
    }
  }

  if (!p) {
    p=calloc(1,sizeof(struct peer_state));
    for(int i=0;i<4;i++) p->sid_prefix_bin[i]=msg[i];
    p->sid_prefix=strdup(peer_prefix);
    p->last_message_number=-1;
    fprintf(stderr,"Registering peer %s*\n",p->sid_prefix);
    if (peer_count<MAX_PEERS) {
      peer_records[peer_count++]=p;
    } else {
      // Peer table full.  Do random replacement.
      int n=random()%MAX_PEERS;
      free_peer(peer_records[n]);
      peer_records[n]=p;
    }
  }
  
  // Update time stamp and most recent message from peer
  p->last_message_time=time(0);
  if (!is_retransmission) p->last_message_number=msg_number;

  while(offset<len) {
    if (debug_pieces) {
      fprintf(stderr,"Saw message section with type '%c' (0x%02x) @ offset %d\n",
	      msg[offset],msg[offset],offset);
    }
    switch(msg[offset]) {
    case 'B':
      offset++;
      if (len-offset<BAR_LENGTH) return -2;
      // BAR announcement
      snprintf(bid_prefix,8*2+1,"%02x%02x%02x%02x%02x%02x%02x%02x",
	       msg[offset+0],msg[offset+1],msg[offset+2],msg[offset+3],
	       msg[offset+4],msg[offset+5],msg[offset+6],msg[offset+7]);
      offset+=8;
      version=0;
      for(int i=0;i<8;i++) version|=((long long)msg[offset+i])<<(i*8LL);
      offset+=8;
      snprintf(recipient_prefix,4*2+1,"%02x%02x%02x%02x",
	       msg[offset+0],msg[offset+1],msg[offset+2],msg[offset+3]);
      offset+=4;
      size_byte=msg[offset];
      offset+=1;
      if (debug_pieces)
	fprintf(stderr,
		"Saw a BAR from %s*: %s* version %lld size byte 0x%02x"
		" (we know of %d bundles held by that peer)\n",
		p->sid_prefix,bid_prefix,version,size_byte,p->bundle_count);
      peer_note_bar(p,bid_prefix,version,recipient_prefix,size_byte);
      break;
    case 'L':
      // Length of bundle announcement for receivers
      offset++;
      if (len-offset<(1+8+8+4)) return -3;
      snprintf(bid_prefix,8*2+1,"%02x%02x%02x%02x%02x%02x%02x%02x",
	       msg[offset+0],msg[offset+1],msg[offset+2],msg[offset+3],
	       msg[offset+4],msg[offset+5],msg[offset+6],msg[offset+7]);
      offset+=8;
      version=0;
      for(int i=0;i<8;i++) version|=((long long)msg[offset+i])<<(i*8LL);
      offset+=8;
      offset_compound=0;
      for(int i=0;i<4;i++) offset_compound|=((long long)msg[offset+i])<<(i*8LL);
      offset+=4;
      saw_length(peer_prefix,bid_prefix,version,offset_compound);
      break;
    case 'P': case 'p': case 'Q': case 'q':
      // Skip header character
      above_1mb=0;
      is_end_piece=0;
      if (!(msg[offset]&0x20)) above_1mb=1;
      if (!(msg[offset]&0x01)) is_end_piece=1;
      offset++;
      if (len-offset<(1+8+8+4+1)) return -3;
      snprintf(bid_prefix,8*2+1,"%02x%02x%02x%02x%02x%02x%02x%02x",
	       msg[offset+0],msg[offset+1],msg[offset+2],msg[offset+3],
	       msg[offset+4],msg[offset+5],msg[offset+6],msg[offset+7]);
      offset+=8;
      version=0;
      for(int i=0;i<8;i++) version|=((long long)msg[offset+i])<<(i*8LL);
      offset+=8;
      offset_compound=0;
      for(int i=0;i<6;i++) offset_compound|=((long long)msg[offset+i])<<(i*8LL);
      offset+=4;
      if (above_1mb) offset+=2;
      piece_offset=(offset_compound&0xfffff)|((offset_compound>>12LL)&0xfff00000LL);
      piece_bytes=(offset_compound>>20)&0x7ff;
      piece_is_manifest=offset_compound&0x80000000;

      saw_piece(peer_prefix,bid_prefix,version,piece_offset,piece_bytes,is_end_piece,
		piece_is_manifest,&msg[offset],
		prefix, servald_server,credential);

      offset+=piece_bytes;

      break;
    case 'R':
      // Request for a segment
      {
	char target_sid[4+1+1];
	char bid_prefix[8*2+1+1];
	int bundle_offset=0;
	int is_manifest=0;
	snprintf(target_sid,5,"%02x%02x",msg[offset],msg[offset+1]);
	offset+=2;
	snprintf(bid_prefix,17,"%02x%02x%02x%02x%02x%02x%02x%02x",
		 msg[offset+0],msg[offset+1],msg[offset+2],msg[offset+3],
		 msg[offset+4],msg[offset+5],msg[offset+6],msg[offset+7]);
	offset+=8;
	bundle_offset|=msg[offset++];
	bundle_offset|=msg[offset++]<<8;
	bundle_offset|=msg[offset++]<<16;
	// We can only request segments upto 8MB point in a bundle via this transport!
	// XXX here be dragons
	if (bundle_offset&0x800000) is_manifest=1;
	bundle_offset&=0x7fffff;
	int size_byte=msg[offset++];
	
	if (debug_pull) {
	  fprintf(stderr,"Saw request from SID=%s* BID=%s (size byte $%02x) @ %c%d addressed to SID=%s*\n",
		  peer_prefix,bid_prefix,size_byte,is_manifest?'M':'B',bundle_offset,
		  target_sid);
	}

	
	// Are we the target SID?
	if (!strncasecmp(my_sid,target_sid,4)) {
	  // Yes, it is addressed to us.
	  // See if we have this bundle, and if so, set the appropriate stream offset
	  // and mark the bundle as requested
	  // XXX linear search!
	  for(int i=0;i<bundle_count;i++) {
	    if (!strncasecmp(bid_prefix,bundles[i].bid,16)) {
	      bundles[i].transmit_now=1;
	      // When adjusting the offset, don't adjust it if we are going to reach
	      // that point within a few hundred bytes, as it won't save any time, and
	      // it might just cause confusion and delay because of the latency of us
	      // receiving the message and responding to it.
	      if (is_manifest) {
		if ((offset<bundles[i].last_manifest_offset_announced)
		    ||((offset-bundles[i].last_manifest_offset_announced)>500))
		  bundles[i].last_manifest_offset_announced=offset;
	      } else {
		if ((offset<bundles[i].last_offset_announced)
		    ||((offset-bundles[i].last_offset_announced)>500))
		bundles[i].last_offset_announced=offset;
	      }
	    }
	  }
	}
      }
      break;
    default:
      // invalid message field.
      return -1;
    }
  }
  return 0;
}
