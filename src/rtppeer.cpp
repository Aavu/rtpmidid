/**
 * Real Time Protocol Music Industry Digital Interface Daemon
 * Copyright (C) 2019 David Moreno Montero <dmoreno@coralbits.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "./logger.hpp"
#include "./rtppeer.hpp"
#include "./exceptions.hpp"
#include "./poller.hpp"
#include "./netutils.hpp"

using namespace rtpmidid;

bool is_command(parse_buffer_t &);

rtppeer::rtppeer(std::string _name, int startport) : local_base_port(startport), name(std::move(_name)) {
  try {
    remote_base_port = 0; // Not defined
    control_socket = -1;
    midi_socket = -1;
    remote_ssrc = 0;

    struct sockaddr_in servaddr;

    control_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (control_socket < 0){
      throw rtpmidid::exception("Can not open control socket. Out of sockets?");
    }
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(startport);
    if (bind(control_socket, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0){
      throw rtpmidid::exception("Can not open control socket. Maybe addres is in use?");
    }
    if (local_base_port == 0){
      socklen_t len = sizeof(servaddr);
      ::getsockname(control_socket, (struct sockaddr*)&servaddr, &len);
      local_base_port = servaddr.sin_port;
      poller.add_fd_in(control_socket, [this](int){ this->control_data_ready(); });
      DEBUG("Got automatic port {} for control", local_base_port);
    }

    midi_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (midi_socket < 0){
      throw rtpmidid::exception("Can not open MIDI socket. Out of sockets?");
    }
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(local_base_port + 1);
    if (bind(midi_socket, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0){
      throw rtpmidid::exception("Can not open MIDI socket. Maybe addres is in use?");
    }
    poller.add_fd_in(midi_socket, [this](int){ this->midi_data_ready(); });

  } catch (...){
    if (control_socket){
      poller.remove_fd(control_socket);
      close(control_socket);
      control_socket = 0;
    }
    if (midi_socket){
      poller.remove_fd(midi_socket);
      close(midi_socket);
      midi_socket = 0;
    }
    throw;
  }
}

rtppeer::~rtppeer(){
  if (control_socket > 0){
    poller.remove_fd(control_socket);
    close(control_socket);
  }
  if (midi_socket > 0){
    poller.remove_fd(midi_socket);
    close(midi_socket);
  }
}

void rtppeer::control_data_ready(){
  uint8_t raw[1500];
  struct sockaddr_in cliaddr;
  unsigned int len = 0;
  auto n = recvfrom(control_socket, raw, 1500, MSG_DONTWAIT, (struct sockaddr *) &cliaddr, &len);
  DEBUG("Got some data from control: {}", n);
  auto buffer = parse_buffer_t(raw, n);

  if (is_command(buffer)){
    parse_command(buffer, control_socket);
  }

  buffer.print_hex(true);
}

void rtppeer::midi_data_ready(){
  uint8_t raw[1500];
  struct sockaddr_in cliaddr;
  unsigned int len = 0;
  auto n = recvfrom(midi_socket, raw, 1500, MSG_DONTWAIT, (struct sockaddr *) &cliaddr, &len);
  DEBUG("Got some data from midi: {}", len);
  auto buffer = parse_buffer_t(raw, n);

  if (is_command(buffer)){
    parse_command(buffer, control_socket);
  }

  buffer.print_hex(true);
}


bool is_command(parse_buffer_t &pb){
  DEBUG("Is command? {} {} {}", pb.size() >= 16, pb.start[0] == 0xFF, pb.start[1] == 0xFF);
  return (pb.size() >= 16 && pb.start[0] == 0xFF && pb.start[1] == 0xFF);
}

void rtppeer::parse_command(parse_buffer_t &buffer, int port){
  if (buffer.size() < 16){
    // This should never be reachable, but should help to smart compilers for
    // further size checks
    throw exception("Invalid command packet.");
  }
  // auto _command =
  buffer.read_uint16(); // We already know it is 0xFFFF
  auto command = buffer.read_uint16();
  DEBUG("Got command type {:X}", command);

  switch(command){
    case rtppeer::OK:
      parse_command_ok(buffer, port);
      break;
    default:
      throw not_implemented();
  }
}

void rtppeer::parse_command_ok(parse_buffer_t &buffer, int port){
  auto protocol = buffer.read_uint32();
  auto initiator_id = buffer.read_uint32();
  remote_ssrc = buffer.read_uint32();
  auto name = buffer.read_str0();

  INFO(
    "Got confirmation from {}:{}, initiator_id: {} ({}) ssrc: {}, name: {}",
    name, remote_base_port, initiator_id, this->initiator_id == initiator_id, remote_ssrc, name
  );
}