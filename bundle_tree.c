/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2016 Serval Project Inc.

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
#include <strings.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <assert.h>

#include "sync.h"
#include "lbard.h"

int bundle_calculate_tree_key(uint8_t bundle_tree_key[SYNC_KEY_LEN],
			      uint8_t sync_tree_salt[SYNC_SALT_LEN],
			      char *bid,
			      long long version,
			      long long length,
			      char *filehash)
{
  /*
    Calculate a sync key for this bundle.
    Sync keys are relatively short, only 64 bits, as this is still sufficient to
    maintain a very low probability of colissions, provided that each peer has less
    than 2^32 bundles.

    Ideally we would hash the entire manifest of a bundle, but that would require us
    to retrieve each manifest, and we would rather not require that.  So instead we
    will use the BID, length, version and filehash as inputs.  This combination means
    that in the unlikely event of a colission, updating the bundle is almost certain
    to resolve the colission.  Thus the natural human response of sending another
    message if the first doesn't get through is likely to resolve the problem.

    The relatively small key space does however leave us potentially vulnerable to a
    determined adversary to find coliding hashes to disrupt communications. We will
    need to do something about this in due course. This could probably be protected
    by using a random salt between pairs of peers when they do the sync process, so
    that colissions cannot be arranged ahead of time.

    Using a salt, and changing it periodically, would also provide implicit protection
    against colissions of any source, so it is probably a really good idea to
    implement.  The main disadvantage is that we need to calculate all the hashes for
    all the bundles we hold whenever we talk to a new peer.  We could, however,
    use a salt for all peers which we update periodically, to offer a good compromise
    between computational cost and protection against accidental and intentional
    colissions.
    
    Under this strategy, we need to periodically recalculate the sync key, i.e, hash,
    for each bundle we hold, and invalidate the sync tree for each peer when we do so.

    ... HOWEVER, we need both sides of a conversation to have the same salt, which
    wouldn't work under that scheme.

    So for now we will employ a salt, but it will probably be fixed until we decide on
    a good solution to this set of problems.
  */

  char lengthstring[80];
  snprintf(lengthstring,80,"%llx:%llx",length,version);
  
  struct sha1nfo sha1;  
  sha1_init(&sha1);
  sha1_write(&sha1,(const char *)sync_tree_salt,SYNC_SALT_LEN);
  sha1_write(&sha1,bid,strlen(bid));
  sha1_write(&sha1,filehash,strlen(filehash));
  sha1_write(&sha1,lengthstring,strlen(lengthstring));
  bcopy(sha1_result(&sha1),bundle_tree_key,SYNC_KEY_LEN);
  return 0;  
}

int sync_update_peer_sequence_acknowledgement_field(int peer,uint8_t *msg)
{
  int len=5;
  // Acknowledge what we have seen from the remote side
  msg[len++]=peer_records[peer]->last_remote_sequence_acknowledged;
  msg[len++]=peer_records[peer]->remote_sequence_bitmap&0xff;
  msg[len++]=(peer_records[peer]->remote_sequence_bitmap>>8)&0xff;
  return 0;
}

int sync_peer_window_has_space(int peer)
{
  int space=peer_records[peer]->last_local_sequence_number
    -peer_records[peer]->last_local_sequence_number_acknowledged;
  if (space<0) space+=256;
  if (space>0) return 1; else return 0;
}

int sync_tree_send_message(int *offset,int mtu, unsigned char *msg_out,int peer)
{         
  uint8_t msg[256];
  int len=0;
  msg[len++]='S'; // Sync message
  // SID prefix of recipient
  msg[len++]=peer_records[peer]->sid_prefix_bin[0];
  msg[len++]=peer_records[peer]->sid_prefix_bin[1];
  msg[len++]=peer_records[peer]->sid_prefix_bin[2];
  // Sequence number (our side)
  peer_records[peer]->last_local_sequence_number++;
  peer_records[peer]->last_local_sequence_number&=0xff;
  msg[len++]=peer_records[peer]->last_local_sequence_number;
  // Acknowledge what we have seen from the remote side
  msg[len++]=peer_records[peer]->last_remote_sequence_acknowledged;
  msg[len++]=peer_records[peer]->remote_sequence_bitmap&0xff;
  msg[len++]=(peer_records[peer]->remote_sequence_bitmap>>8)&0xff;
  int used=sync_build_message(&peer_records[peer]->sync_state,
			      &msg[len],256-len);
  len+=used;
  append_bytes(offset,mtu,msg_out,msg,len);
  return 0;
}

int sync_append_some_bundle_bytes(int bundle_number,int start_offset,int len,
				  unsigned char *p, int is_manifest,
				  int *offset,int mtu,unsigned char *msg)
{
  int max_bytes=mtu-(*offset)-21;
  int bytes_available=len-start_offset;
  int actual_bytes=0;
  int end_of_item=0;

  // If we can't announce even one byte, we should just give up.
  if (start_offset>0xfffff) {
    max_bytes-=2; if (max_bytes<0) max_bytes=0;
  }
  if (max_bytes<1) return -1;

  // Work out number of bytes to include in announcement
  if (bytes_available<max_bytes) {
    actual_bytes=bytes_available;
    end_of_item=0;
  } else {
    actual_bytes=max_bytes;
    end_of_item=1;
  }
  // Make sure byte count fits in 11 bits.
  if (actual_bytes>0x7ff) actual_bytes=0x7ff;

  if (actual_bytes<0) return -1;
  
  // Generate 4 byte offset block (and option 2-byte extension for big bundles)
  long long offset_compound=0;
  offset_compound=(start_offset&0xfffff);
  offset_compound|=((actual_bytes&0x7ff)<<20);
  if (is_manifest) offset_compound|=0x80000000;
  offset_compound|=((start_offset>>20LL)&0xffffLL)<<32LL;

  // Now write the 21/23 byte header and actual bytes into output message
  // BID prefix (8 bytes)
  if (start_offset>0xfffff)
    msg[(*offset)++]='P'+end_of_item;
  else 
    msg[(*offset)++]='p'+end_of_item;
  
  
  for(int i=0;i<8;i++)
    msg[(*offset)++]=hex_byte_value(&bundles[bundle_number].bid[i*2]);
  // Bundle version (8 bytes)
  for(int i=0;i<8;i++)
    msg[(*offset)++]=(cached_version>>(i*8))&0xff;
  // offset_compound (4 bytes)
  for(int i=0;i<4;i++)
    msg[(*offset)++]=(offset_compound>>(i*8))&0xff;
  if (start_offset>0xfffff) {
  for(int i=4;i<6;i++)
    msg[(*offset)++]=(offset_compound>>(i*8))&0xff;
  }

  bcopy(p,&msg[(*offset)],actual_bytes);
  (*offset)+=actual_bytes;

  if (debug_announce) {
    fprintf(stderr,"Announcing ");
    for(int i=0;i<8;i++) fprintf(stderr,"%c",bundles[bundle_number].bid[i]);
    fprintf(stderr,"* (priority=0x%llx) version %lld %s segment [%d,%d)\n",
	    bundles[bundle_number].last_priority,
	    bundles[bundle_number].version,
	    is_manifest?"manifest":"payload",
	    start_offset,start_offset+actual_bytes);
  }

  char status_msg[1024];
  snprintf(status_msg,1024,"Announcing %c%c%c%c%c%c%c%c* version %lld %s segment [%d,%d)",
	   bundles[bundle_number].bid[0],bundles[bundle_number].bid[1],
	   bundles[bundle_number].bid[2],bundles[bundle_number].bid[3],
	   bundles[bundle_number].bid[4],bundles[bundle_number].bid[5],
	   bundles[bundle_number].bid[6],bundles[bundle_number].bid[7],
	   bundles[bundle_number].version,
	   is_manifest?"manifest":"payload",
	   start_offset,start_offset+actual_bytes);
  status_log(status_msg);

  return actual_bytes;
}


int sync_announce_bundle_piece(int peer,int *offset,int mtu,
			       unsigned char *msg,
			       char *sid_prefix_hex,
			       char *servald_server, char *credential)
{
  int bundle_number=peer_records[peer]->tx_bundle;

  if (prime_bundle_cache(bundle_number,
			 sid_prefix_hex,servald_server,credential)) return -1;
  
  if (peer_records[peer]->tx_bundle_manifest_offset<1024) {
    // Send piece of manifest
    int start_offset=peer_records[peer]->tx_bundle_manifest_offset;
    int bytes =
      sync_append_some_bundle_bytes(bundle_number,start_offset,
				    cached_manifest_encoded_len,
				    &cached_manifest_encoded[start_offset],1,
				    offset,mtu,msg);
    if (bytes>0)
      peer_records[peer]->tx_bundle_manifest_offset+=bytes;
    // Mark manifest all sent once we get to the end
    if (peer_records[peer]->tx_bundle_manifest_offset>=cached_manifest_encoded_len)
      peer_records[peer]->tx_bundle_manifest_offset=1024;

  }

  // Announce the length of the body if we have finished sending the manifest,
  // but not yet started on the body.  This is really just to help monitoring
  // the progress of transfers for debugging.  The transfer process will automatically
  // detect the end of the bundle when the last piece is received.
  if (peer_records[peer]->tx_bundle_manifest_offset>=1024) {
    // Send length of body?
    if (!peer_records[peer]->tx_bundle_body_offset) {
      if ((mtu-*offset)>(1+8+8+4)) {
	// Announce length of bundle
	msg[(*offset)++]='L';
	// Bundle prefix (8 bytes)
	for(int i=0;i<8;i++)
	  msg[(*offset)++]=hex_byte_value(&bundles[bundle_number].bid[i*2]);
	// Bundle version (8 bytes)
	for(int i=0;i<8;i++)
	  msg[(*offset)++]=(cached_version>>(i*8))&0xff;
	// Length (4 bytes)
	msg[(*offset)++]=(bundles[bundle_number].length>>0)&0xff;
	msg[(*offset)++]=(bundles[bundle_number].length>>8)&0xff;
	msg[(*offset)++]=(bundles[bundle_number].length>>16)&0xff;
	msg[(*offset)++]=(bundles[bundle_number].length>>24)&0xff;
      }
    } else {
      // Send some of the body
      int start_offset=peer_records[peer]->tx_bundle_body_offset;
      int bytes =
	sync_append_some_bundle_bytes(bundle_number,start_offset,cached_body_len,
				      &cached_body[start_offset],0,
				      offset,mtu,msg);

      if (bytes>0)
	peer_records[peer]->tx_bundle_body_offset+=bytes;      
    }
  }
  return 0;
}

int sync_tree_send_data(int *offset,int mtu, unsigned char *msg_out,int peer,
			char *sid_prefix_bin,char *servald_server,char *credential)
{
  /*
    Send a piece of the bundle (manifest or body) to this peer, for the highest
    priority bundle that we have that we believe that they don't have.
    (If they have it, then they will acknowledge the entirety of it, allowing us
    to advance to the next bundle.)

   */
  if (peer_records[peer]->tx_bundle>-1)
    {
      // Try to also send a piece of body, even if we have already stuffed some
      // manifest in, because we might still have space.
      
      sync_announce_bundle_piece(peer,offset,mtu,msg_out,
				 sid_prefix_bin,servald_server,credential);
    }
  return 0;
}

int sync_by_tree_stuff_packet(int *offset,int mtu, unsigned char *msg_out,
			      char *sid_prefix_bin,
			      char *servald_server,char *credential)
{
  // Stuff packet as full as we can with data for as many peers as we can.
  // In practice, we will likely fill it on the first peer, but let's not
  // waste a packet if we have something we can stuff in.
  int count=10;
  while((*offset)<(mtu-16)) {
    if ((count--)<0) break;
    int peer=random_active_peer();
    if (peer_records[peer]->retransmit_requested) {
      // Retransmit last transmission.
      int slot=peer_records[peer]->retransmition_sequence&15;
      // Update acknowledgement bytes to reflect current situation.
      sync_update_peer_sequence_acknowledgement_field
	(peer,peer_records[peer]->retransmit_buffer[slot]);
      if (!append_bytes(offset,mtu,msg_out,
			peer_records[peer]->retransmit_buffer[slot],
			peer_records[peer]->retransmit_lengths[slot]))
	peer_records[peer]->retransmit_requested=0;
    }
    int space=mtu-(*offset);
    if (sync_peer_window_has_space(peer)&&(space>10)) {
      /* Try sending something new.
	 Sync trees, and if space remains (because we have synchronised trees),
	 then try sending a piece of a bundle, if any */
      sync_tree_send_message(offset,mtu,msg_out,peer);
      sync_tree_send_data(offset,mtu,msg_out,peer,
			  sid_prefix_bin,servald_server,credential);
    }
  }  
  
  return 0;
}

int sync_tree_prepare_tree(int peer)
{
  // Default fixed salt.
  uint8_t sync_tree_salt[SYNC_SALT_LEN]={0xa9,0x1b,0x8d,0x11,0xdd,0xee,0x20,0xd0};

  // Clear tree
  bzero(&peer_records[peer]->sync_state,sizeof(peer_records[peer]->sync_state));
  
  for(int i=0;i<bundle_count;i++) {
    sync_key_t key;
    bundle_calculate_tree_key(key.key,
			      sync_tree_salt,
			      bundles[i].bid,
			      bundles[i].version,
			      bundles[i].length,
			      bundles[i].filehash);
    sync_add_key(&peer_records[peer]->sync_state,&key);
  }
  return 0;
}

XXX - Implement quasi-reliable receipt of packets from peers (out of order delivery is fine, provided that we get the omitted frames eventually. lost frames affect efficiency, not completeness of procedure)

XXX - Implement sending of bundle pieces based on sync data discoveries.

XXX - Implement telling sender when they send data that we already have.