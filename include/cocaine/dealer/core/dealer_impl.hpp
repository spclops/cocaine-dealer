/*
    Copyright (c) 2011-2012 Rim Zaidullin <creator@bash.org.ru>
    Copyright (c) 2011-2012 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>. 
*/

#ifndef _COCAINE_DEALER_CLIENT_IMPL_HPP_INCLUDED_
#define _COCAINE_DEALER_CLIENT_IMPL_HPP_INCLUDED_

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <zmq.hpp>

#include <boost/function.hpp>
#include <boost/date_time.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/xpressive/xpressive.hpp>

#include "cocaine/dealer/forwards.hpp"
#include "cocaine/dealer/message.hpp"
#include "cocaine/dealer/core/context.hpp"
#include "cocaine/dealer/core/service.hpp"
#include "cocaine/dealer/core/dealer_object.hpp"
#include "cocaine/dealer/heartbeats/overseer.hpp"

namespace cocaine {
namespace dealer {

class dealer_impl_t :
	private boost::noncopyable,
	public dealer_object_t
{
public:
	typedef boost::shared_ptr<response_t> response_ptr_t;
	typedef std::vector<response_ptr_t> responses_list_t;

	typedef boost::shared_ptr<service_t> service_ptr_t;
	typedef std::vector<cocaine_endpoint_t> cocaine_endpoints_list_t;

	typedef std::map<std::string, service_ptr_t> services_map_t;
	typedef std::map<std::string, cocaine_endpoints_list_t> handles_endpoints_t;

public:
	explicit dealer_impl_t(const std::string& config_path);
	virtual ~dealer_impl_t();

	response_ptr_t
	send_message(const message_t& message);

	response_ptr_t
	send_message(const void* data,
				 size_t size,
				 const message_path_t& path,
				 const message_policy_t& policy);

	response_ptr_t
	send_message(const void* data,
				 size_t size,
				 const message_path_t& path);

	
	responses_list_t
	send_messages(const void* data,
				  size_t size,
				  const message_path_t& path,
				  const message_policy_t& policy);

	responses_list_t
	send_messages(const void* data,
				  size_t size,
				  const message_path_t& path);

	boost::shared_ptr<message_iface>
	create_message(const void* data,
				   size_t size,
				   const message_path_t& path,
				   const message_policy_t& policy);

	message_policy_t policy_for_service(const std::string& service_alias);

	size_t stored_messages_count(const std::string& service_alias);
	void remove_stored_message(const message_t& message);
	void remove_stored_message_for(const response_ptr_t& response);
	void get_stored_messages(const std::string& service_alias,
							 std::vector<message_t>& messages);

private:	
	void connect();
	void disconnect();

	void process_overseer_event(e_overseer_event event_type,
								const std::string& service_name,
								const std::string& handle_name,
								const std::set<cocaine_endpoint_t>& endpoints);

	// restoring messages from storage cache
	void storage_iteration_callback(const std::string& key, void* data, uint64_t size, int column);

	bool regex_match(const std::string& regex_str, const std::string& value);

	boost::shared_ptr<service_t> get_service(const std::string& service_alias);

private:
	std::map<std::string, boost::xpressive::sregex> m_regex_cache;

	size_t m_messages_cache_size;

	// dealer service name mapped to service
	services_map_t m_services;

	std::auto_ptr<overseer_t> m_overseer;

	// synchronization
	boost::mutex m_mutex;
	boost::mutex m_regex_mutex;

	// alive state
	bool m_is_dead;

	// tmp var to fill with unsent data. used in eblob iterator method (this is god awful code, needs proper fix)
	boost::mutex m_messages_ptr_mutex;
	std::vector<message_t>* m_messages_ptr;
};

} // namespace dealer
} // namespace cocaine

#endif // _COCAINE_DEALER_CLIENT_IMPL_HPP_INCLUDED_
