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

#include <stdexcept>

#include <boost/current_function.hpp>

#include "cocaine/dealer/core/cached_message.hpp"
#include "cocaine/dealer/core/request_metadata.hpp"
#include "cocaine/dealer/core/persistent_data_container.hpp"
#include "cocaine/dealer/utils/data_container.hpp"
#include "cocaine/dealer/utils/error.hpp"
#include "cocaine/dealer/heartbeats/http_hosts_fetcher.hpp"
#include "cocaine/dealer/heartbeats/file_hosts_fetcher.hpp"
#include "cocaine/dealer/storage/eblob_storage.hpp"
#include "cocaine/dealer/response.hpp"

#include "cocaine/dealer/core/dealer_impl.hpp"

namespace cocaine {
namespace dealer {

typedef cached_message_t<persistent_data_container, persistent_request_metadata_t> p_message_t;

dealer_impl_t::dealer_impl_t(const std::string& config_path) :
	m_messages_ptr(NULL)
{
	// create dealer context
	std::string ctx_error_msg = "could not create dealer context, ";

	char* absolute_config_path;
	std::string config_path_tmp = config_path;

	absolute_config_path = realpath(config_path_tmp.c_str(), NULL);
    if (NULL != absolute_config_path) {
    	config_path_tmp = absolute_config_path;
    }

	try {
		boost::shared_ptr<cocaine::dealer::context_t> ctx;
		ctx.reset(new cocaine::dealer::context_t(config_path_tmp));
		ctx->create_storage();
		set_context(ctx);
	}
	catch (const std::exception& ex) {
		throw internal_error(ctx_error_msg + ex.what());
	}

	boost::function<void()> f = boost::bind(&dealer_impl_t::main_loop, this);
	m_thread = boost::thread(f);
}

dealer_impl_t::~dealer_impl_t() {
	m_terminate->send();
	m_thread.join();
	log(PLOG_INFO, "dealer destroyed.");
}

void
dealer_impl_t::terminate(ev::async& as, int type) {
	// kill terminator
	assert(m_terminate);
	m_terminate->stop();
	m_terminate.reset();

	// kill overseer
	assert(m_overseer);
	m_overseer->stop();
	m_overseer.reset();

	// kill services
	for (auto it = m_services.begin(); it != m_services.end(); ++it) {
		assert(it->second);
		it->second.reset();
	}
	m_services.clear();

	context()->event_loop().unloop(ev::ALL);
}

void
dealer_impl_t::main_loop() {
	log(PLOG_INFO, "creating dealer.");

	// create terminator
	ev::async* async = new ev::async(context()->event_loop());
	m_terminate.reset(async);
	m_terminate->set<dealer_impl_t, &dealer_impl_t::terminate>(this);
	m_terminate->start();

	// create services
	const configuration_t::services_list_t& services_info_list = config()->services_list();
	for (auto it = services_info_list.begin(); it != services_info_list.end(); ++it) {
		boost::shared_ptr<service_t> service_ptr(new service_t(it->second, context()));
		log(PLOG_INFO, "STARTING NEW SERVICE [%s]", it->second.name.c_str());
		m_services[it->first] = service_ptr;
	}

	// create overseer
	m_overseer.reset(new overseer_t(context()));
	m_overseer->set_callback(boost::bind(&dealer_impl_t::process_overseer_event, this, _1, _2, _3, _4));
	m_overseer->run();

	log(PLOG_INFO, "dealer created.");

	context()->event_loop().loop();
}

void
dealer_impl_t::process_overseer_event(e_overseer_event event_type,
									  const std::string& service_name,
									  const std::string& handle_name,
									  const std::set<cocaine_endpoint_t>& endpoints)
{
	std::cout << "process_overseer_event\n";

	/*
	// find corresponding service
	services_map_t::iterator it = m_services.find(service_name);

	// populate service with pinged hosts and handles
	if (it != m_services.end()) {
		service_ptr_t service = it->second;
		assert(service);
		
		handle_info_t info(handle_name, service->info().app, service_name);

		switch (event_type) {
			case CREATE_HANDLE:
				service->create_handle(info, endpoints);
				break;

			case UPDATE_HANDLE:
				service->update_handle(info, endpoints);
				break;

			case DESTROY_HANDLE:
				service->destroy_handle(info);
				break;
		}
	}
	else {
		std::string error_msg = "service with name " + service_name;
		error_msg += " was not found in services. at: ";
		throw internal_error(error_msg);
	}
	*/
}

boost::shared_ptr<service_t>
dealer_impl_t::get_service(const std::string& service_alias) {
	const static std::string error_str = "can't sent message. no service with name %s found.";

	// find service to send message to
	services_map_t::iterator it = m_services.find(service_alias);

	if (it == m_services.end()) {
		throw dealer_error(location_error,
						   error_str,
						   service_alias.c_str());
	}

	boost::shared_ptr<service_t> service_ptr = it->second;
	assert(service_ptr);

	if (service_ptr->is_dead()) {
		throw dealer_error(location_error,
						   error_str,
						   service_alias.c_str());
	}

	return service_ptr;
}

boost::shared_ptr<response_t>
dealer_impl_t::send_message(const message_t& message) {
	return dealer_impl_t::send_message(message.data.data(),
									   message.data.size(),
									   message.path,
									   message.policy);	
}

boost::shared_ptr<response_t>
dealer_impl_t::send_message(const void* data,
							size_t size,
							const message_path_t& path)
{
	boost::shared_ptr<service_t> service = get_service(path.service_alias);
	return dealer_impl_t::send_message(data, size, path, service->info().policy);
}

boost::shared_ptr<response_t>
dealer_impl_t::send_message(const void* data,
							size_t size,
							const message_path_t& path,
							const message_policy_t& policy)
{	
	//boost::mutex::scoped_lock lock(m_mutex);
	boost::shared_ptr<service_t> service = get_service(path.service_alias);
	boost::shared_ptr<message_iface> msg = create_message(data, size, path, policy);

	return service->send_message(msg);
}

std::vector<boost::shared_ptr<response_t> >
dealer_impl_t::send_messages(const void* data,
							 size_t size,
							 const message_path_t& path,
							 const message_policy_t& policy)
{
	std::vector<boost::shared_ptr<response_t> > responces_list;

	const configuration_t::services_list_t& services_info_list = config()->services_list();
	configuration_t::services_list_t::const_iterator it = services_info_list.begin();
	for (; it != services_info_list.end(); ++it) {
		if (regex_match(path.service_alias, it->second.name)) {
			message_path_t exact_path = path;
			exact_path.service_alias = it->second.name; 

			boost::shared_ptr<response_t> resp = send_message(data, size, exact_path, policy);
			responces_list.push_back(resp);
		}
	}
	
	return responces_list;
}

std::vector<boost::shared_ptr<response_t> >
dealer_impl_t::send_messages(const void* data,
							 size_t size,
							 const message_path_t& path)
{
	std::vector<boost::shared_ptr<response_t> > responces_list;

	const configuration_t::services_list_t& services_info_list = config()->services_list();
	configuration_t::services_list_t::const_iterator it = services_info_list.begin();
	for (; it != services_info_list.end(); ++it) {
		if (regex_match(path.service_alias, it->second.name)) {
			message_path_t exact_path = path;
			exact_path.service_alias = it->second.name; 

			boost::shared_ptr<response_t> resp = send_message(data, size, exact_path);
			responces_list.push_back(resp);
		}
	}
	
	return responces_list;
}

message_policy_t
dealer_impl_t::policy_for_service(const std::string& service_alias) {
	boost::shared_ptr<service_t> service;

	try {
		service = get_service(service_alias);
	}
	catch (...) {
		return message_policy_t();
	}

	return service->info().policy;
}

bool
dealer_impl_t::regex_match(const std::string& regex_str, const std::string& value) {
	boost::mutex::scoped_lock lock(m_regex_mutex);
	
	std::map<std::string, boost::xpressive::sregex>::iterator it;
	it = m_regex_cache.find(regex_str);

	boost::xpressive::sregex rex;
	boost::xpressive::smatch what;

	if (it != m_regex_cache.end()) {
		rex = it->second;
	}
	else {
		rex = boost::xpressive::sregex::compile(regex_str);
		m_regex_cache[regex_str] = rex;
	}

	return boost::xpressive::regex_match(value, what, rex);
}

void
dealer_impl_t::disconnect() {

}

boost::shared_ptr<message_iface>
dealer_impl_t::create_message(const void* data,
							  size_t size,
							  const message_path_t& path,
							  const message_policy_t& policy)
{
	typedef cached_message_t<data_container, request_metadata_t> msg_t;
	boost::shared_ptr<message_iface> msg(new msg_t(path,
												   policy,
												   data,
												   size));

	if (config()->message_cache_type() == PERSISTENT &&
		policy.persistent == true)
	{
		boost::shared_ptr<eblob_t> eb = context()->storage()->get_eblob(path.service_alias);
		msg->commit_to_eblob(eb);
		log(PLOG_DEBUG,
			"commited message with uuid: %s to persistent storage.",
			msg->uuid().as_human_readable_string().c_str());
	}

	return msg;
}

size_t
dealer_impl_t::stored_messages_count(const std::string& service_alias) {
	if (config()->message_cache_type() != PERSISTENT) {
		return 0;
	}

	boost::shared_ptr<eblob_t> blob = this->context()->storage()->get_eblob(service_alias);
	return blob->alive_items_count();
}

void
dealer_impl_t::get_stored_messages(const std::string& service_alias,
								   std::vector<message_t>& messages)
{
	if (config()->message_cache_type() != PERSISTENT) {
		return;
	}

	boost::shared_ptr<eblob_t> blob = this->context()->storage()->get_eblob(service_alias);
	int unsent_messages_count = blob->alive_items_count();

	assert(unsent_messages_count >= 0);

	if (unsent_messages_count == 0) {
		std::string log_str = "no messages to restore for service [%s] from persistent cache...";
		log(PLOG_DEBUG, log_str, service_alias.c_str());
		return;
	}

	std::string log_str = "restoring %d messages for service [%s] from persistent cache...";
	log(PLOG_DEBUG, log_str, unsent_messages_count, service_alias.c_str());

	m_messages_ptr = &messages;

	eblob_t::iteration_callback_t callback;
	callback = boost::bind(&dealer_impl_t::storage_iteration_callback, this, _1, _2, _3, _4);
	blob->iterate(callback);

	m_messages_ptr = NULL;
}

void
dealer_impl_t::remove_stored_message(const message_t& message) {
	if (config()->message_cache_type() != PERSISTENT) {
		return;
	}

	boost::shared_ptr<eblob_t> blob;
	blob = this->context()->storage()->get_eblob(message.path.service_alias);
	blob->remove_all(message.id);
}

void
dealer_impl_t::remove_stored_message_for(const response_ptr_t& response) {
}

void
dealer_impl_t::storage_iteration_callback(const std::string& key,
										  void* data,
										  uint64_t size,
										  int column)
{
	if (!m_messages_ptr) {
		return;
	}

	if (column == 0 && (!data || size == 0)) {
		throw internal_error("empty message found in cache: " + std::string(BOOST_CURRENT_FUNCTION));
	}

    // init unpacker
	msgpack::unpacker pac;
	pac.reserve_buffer(size);
	memcpy(pac.buffer(), data, size);
	pac.buffer_consumed(size);

	message_t msg;
	msgpack::unpacked result;

    pac.next(&result);
    result.get().convert(&msg.path);

	pac.next(&result);
    result.get().convert(&msg.policy);

    pac.next(&result);
    result.get().convert(&msg.id);

    std::string msg_data;
    pac.next(&result);
    result.get().convert(&msg_data);
    msg.data.set_data(msg_data.data(), msg_data.size());

    if (m_messages_ptr) {
		boost::mutex::scoped_lock lock(m_messages_ptr_mutex);
    	m_messages_ptr->push_back(msg);
	}
}

} // namespace dealer
} // namespace cocaine
