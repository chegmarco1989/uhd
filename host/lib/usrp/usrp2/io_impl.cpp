//
// Copyright 2010 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "usrp2_impl.hpp"
#include "usrp2_regs.hpp"
#include <uhd/transport/convert_types.hpp>
#include <boost/format.hpp>
#include <boost/asio.hpp> //htonl and ntohl
#include <iostream>

using namespace uhd;
using namespace uhd::usrp;
using namespace uhd::transport;
namespace asio = boost::asio;

/***********************************************************************
 * Helper Functions
 **********************************************************************/
void usrp2_impl::io_init(void){
    //setup rx otw type
    _rx_otw_type.width = 16;
    _rx_otw_type.shift = 0;
    _rx_otw_type.byteorder = otw_type_t::BO_BIG_ENDIAN;

    //setup tx otw type
    _tx_otw_type.width = 16;
    _tx_otw_type.shift = 0;
    _tx_otw_type.byteorder = otw_type_t::BO_BIG_ENDIAN;

    //send a small data packet so the usrp2 knows the udp source port
    managed_send_buffer::sptr send_buff = _data_transport->get_send_buff();
    boost::uint32_t data = htonl(USRP2_INVALID_VRT_HEADER);
    memcpy(send_buff->cast<void*>(), &data, sizeof(data));
    send_buff->done(sizeof(data));

    //setup RX DSP regs
    std::cout << "RX samples per packet: " << max_rx_samps_per_packet() << std::endl;
    _iface->poke32(FR_RX_CTRL_NSAMPS_PER_PKT, max_rx_samps_per_packet());
    _iface->poke32(FR_RX_CTRL_NCHANNELS, 1);
    _iface->poke32(FR_RX_CTRL_CLEAR_OVERRUN, 1); //reset
    _iface->poke32(FR_RX_CTRL_VRT_HEADER, 0
        | (0x1 << 28) //if data with stream id
        | (0x1 << 26) //has trailer
        | (0x3 << 22) //integer time other
        | (0x1 << 20) //fractional time sample count
    );
    _iface->poke32(FR_RX_CTRL_VRT_STREAM_ID, 0);
    _iface->poke32(FR_RX_CTRL_VRT_TRAILER, 0);
}

/***********************************************************************
 * Send Data
 **********************************************************************/
size_t usrp2_impl::send(
    const asio::const_buffer &buff,
    const tx_metadata_t &metadata_,
    const io_type_t &io_type
){
    tx_metadata_t metadata = metadata_; //rw copy to change later

    transport::managed_send_buffer::sptr send_buff = _data_transport->get_send_buff();
    boost::uint32_t *tx_mem = send_buff->cast<boost::uint32_t *>();
    size_t num_samps = std::min(std::min(
        asio::buffer_size(buff)/io_type.size,
        size_t(max_tx_samps_per_packet())),
        send_buff->size()/io_type.size
    );

    //kill the end of burst flag if this is a fragment
    if (asio::buffer_size(buff)/io_type.size < num_samps)
        metadata.end_of_burst = false;

    size_t num_header_words32, num_packet_words32;
    size_t packet_count = _tx_stream_id_to_packet_seq[metadata.stream_id]++;

    //pack metadata into a vrt header
    vrt::pack(
        metadata,            //input
        tx_mem,              //output
        num_header_words32,  //output
        num_samps,           //input
        num_packet_words32,  //output
        packet_count,        //input
        get_master_clock_freq()
    );

    boost::uint32_t *items = tx_mem + num_header_words32; //offset for data

    //copy-convert the samples into the send buffer
    convert_io_type_to_otw_type(
        asio::buffer_cast<const void*>(buff), io_type,
        (void*)items, _tx_otw_type,
        num_samps
    );

    //send and return number of samples
    send_buff->done(num_packet_words32*sizeof(boost::uint32_t));
    return num_samps;
}

/***********************************************************************
 * Receive Data
 **********************************************************************/
size_t usrp2_impl::recv(
    const asio::mutable_buffer &buff,
    rx_metadata_t &metadata,
    const io_type_t &io_type
){
    return vrt_packet_handler::recv(
        _packet_handler_recv_state, //last state of the recv handler
        buff, metadata,             //buffer to fill and samples metadata
        io_type, _rx_otw_type,      //input and output types to convert
        get_master_clock_freq(),    //master clock tick rate
        _data_transport             //zero copy interface
    );
}
