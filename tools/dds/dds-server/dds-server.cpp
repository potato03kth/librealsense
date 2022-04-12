// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2022 Intel Corporation. All Rights Reserved.

#include <iostream>

#include "dds-server.h"

#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/qos/PublisherQos.hpp>
#include <fastdds/rtps/participant/ParticipantDiscoveryInfo.h>
#include <src/dds/msg/devicesPubSubTypes.h>

// We align the DDS topic name to ROS2 as it expect the 'rt/' prefix for the topic name
#define ROS2_PREFIX( name ) std::string( "rt/" ).append( name )

using namespace eprosima::fastdds::dds;

dds_server::dds_server()
    : _running( false )
    , _participant( nullptr )
    , _publisher( nullptr )
    , _topic( nullptr )
    , _type_support_ptr( new devicesPubSubType() )
    , _dds_device_dispatcher( 10 )
    , _ctx( "{"
            "\"dds-discovery\" : false"
            "}" )
{
}
bool dds_server::init( DomainId_t domain_id )
{
    DomainParticipantQos pqos;
    pqos.name( "rs-dds-server" );
    _participant
        = DomainParticipantFactory::get_instance()->create_participant( domain_id,
                                                                        pqos,
                                                                        &_domain_listener );
    std::cout << "Creating a RS DDS server for domain " << domain_id << std::endl;
    _dds_device_dispatcher.start();
    return _participant != nullptr;
}

void dds_server::run()
{
    // Registering the topic type enables topic instance creation by factory
    _type_support_ptr.register_type( _participant );

    _publisher = _participant->create_publisher( PUBLISHER_QOS_DEFAULT, nullptr );

    if( _publisher == nullptr )
    {
        throw std::runtime_error( "Error creating a DDS devices publisher" );
    }

    _topic = _participant->create_topic( ROS2_PREFIX( "Devices" ),
                                         _type_support_ptr->getName(),
                                         TOPIC_QOS_DEFAULT );

    if( _topic == nullptr )
    {
        throw std::runtime_error( "Error creating a DDS devices topic" );
    }

    _running = true;

    // Query the devices connected on startup
    auto connected_dev_list = _ctx.query_devices();
    std::vector< std::pair< std::string, rs2::device > > devices_to_add;
    for( auto connected_dev : connected_dev_list )
    {
        auto device_name = connected_dev.get_info( RS2_CAMERA_INFO_NAME );
        devices_to_add.push_back( { device_name, connected_dev } );
    }

    // Post the devices connected on startup
    _dds_device_dispatcher.invoke( [this, devices_to_add]( dispatcher::cancellable_timer c ) {
        handle_device_changes( {}, devices_to_add );
    } );

    // Register to LRS device changes function
    _ctx.set_devices_changed_callback( [ this ]( rs2::event_information & info ) {
        if( _running )
        {
            // Remove disconnected devices
            std::vector< std::string > devices_to_remove;
            for( auto dev_info : _devices_writers )
            {
                auto & dev = dev_info.second.device;
                auto device_name = dev.get_info( RS2_CAMERA_INFO_NAME );

                if( info.was_removed( dev ) )
                {
                    devices_to_remove.push_back( device_name );
                }
            }

            // Add new connected devices
            std::vector< std::pair< std::string, rs2::device > > devices_to_add;
            for( auto && dev : info.get_new_devices() )
            {
                auto device_name = dev.get_info( RS2_CAMERA_INFO_NAME );
                devices_to_add.push_back( { device_name, dev } );
            }

            // Post the devices connected / removed
            _dds_device_dispatcher.invoke(
                [this, devices_to_remove, devices_to_add]( dispatcher::cancellable_timer c ) {
                    handle_device_changes( devices_to_remove, devices_to_add );
                } );
        }
    } );

    std::cout << "RS DDS Server is on.." << std::endl;
}

void dds_server::handle_device_changes(
    std::vector< std::string > devices_to_remove,
    std::vector< std::pair< std::string, rs2::device > > devices_to_add )
{
    try
    {
        for( auto dev_to_remove : devices_to_remove )
        {
            _publisher->delete_datawriter( _devices_writers[dev_to_remove].data_writer );
            std::cout << "Device '" << dev_to_remove << "' - removed" << std::endl;
            _devices_writers.erase( dev_to_remove );
        }

        for( auto dev_to_add : devices_to_add )
        {
            auto dev_name = dev_to_add.first;
            auto rs2_dev = dev_to_add.second;

            // Create a data writer for the topic
            DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
            wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
            wqos.durability().kind = VOLATILE_DURABILITY_QOS;
            wqos.data_sharing().automatic();
            wqos.ownership().kind = EXCLUSIVE_OWNERSHIP_QOS;
            std::shared_ptr< dds_serverListener > writer_listener = std::make_shared< dds_serverListener >();

            _devices_writers[dev_name]
                = { rs2_dev,
                    _publisher->create_datawriter( _topic, wqos, writer_listener.get() ),
                    writer_listener };

            if( _devices_writers[dev_name].data_writer == nullptr )
            {
                std::cout << "Error creating a DDS writer" << std::endl;
                return;
            }

            // Publish the device info, but only after a matching reader is found.
            devices msg;
            strcpy( msg.name().data(), dev_name.c_str() );
            std::cout << "\nDevice '" << dev_name << "' - detected" << std::endl;
            std::cout << "Looking for at least 1 matching reader... ";  // Status value will be
                                                                        // appended to this line

            int retry_cnt = 4;
            bool reader_found = false;
            do
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
                reader_found = _devices_writers[dev_name].listener->_matched != 0;
            }
            while( ! reader_found && retry_cnt-- > 0 );


            if( reader_found )
            {
                std::cout << "found" << std::endl;
                if( _devices_writers[dev_name].data_writer->write( &msg ) )
                {
                    std::cout << "DDS device message sent!" << std::endl;
                }
                else
                {
                    std::cout << "Error writing new device message for: " << dev_name << std::endl;
                }
            }
            else
            {
                std::cout << "not found" << std::endl;
                std::cout << "Timeout finding a reader for devices topic" << std::endl;
            }
        }
    }

    catch( ... )
    {
        std::cout << "Unknown error when trying to remove/add a DDS device" << std::endl;
    }
}

dds_server::~dds_server()
{
    std::cout << "Shutting down rs-dds-server..." << std::endl;
    _running = false;

    _dds_device_dispatcher.stop();

    for( auto device_writer : _devices_writers )
    {
        auto & dev_writer = device_writer.second.data_writer;
        if( dev_writer )
            _publisher->delete_datawriter( dev_writer );
    }
    _devices_writers.clear();

    if( _topic != nullptr )
    {
        _participant->delete_topic( _topic );
    }
    if( _publisher != nullptr )
    {
        _participant->delete_publisher( _publisher );
    }
    DomainParticipantFactory::get_instance()->delete_participant( _participant );
}


void dds_server::dds_serverListener::on_publication_matched( DataWriter * writer,
                                                             const PublicationMatchedStatus & info )
{
    if( info.current_count_change == 1 )
    {
        std::cout << "DataReader " << writer->guid() << " discovered" << std::endl;
        _matched = info.total_count;
    }
    else if( info.current_count_change == -1 )
    {
        std::cout << "DataReader " << writer->guid() << " disappeared" << std::endl;
        _matched = info.total_count;
    }
    else
    {
        std::cout << std::to_string( info.current_count_change )
                  << " is not a valid value for on_publication_matched" << std::endl;
    }
}

void dds_server::DiscoveryDomainParticipantListener::on_participant_discovery(
    DomainParticipant * participant, eprosima::fastrtps::rtps::ParticipantDiscoveryInfo && info )
{
    switch( info.status )
    {
    case eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::DISCOVERED_PARTICIPANT:
        std::cout << "Participant '" << info.info.m_participantName << "' discovered" << std::endl;
        break;
    case eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::REMOVED_PARTICIPANT:
    case eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::DROPPED_PARTICIPANT:
        std::cout << "Participant '" << info.info.m_participantName << "' disappeared" << std::endl;
        break;
    default:
        break;
    }
}
