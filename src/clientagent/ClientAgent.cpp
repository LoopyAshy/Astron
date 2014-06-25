#include "ClientAgent.h"
#include "ClientFactory.h"

#include "core/global.h"
#include "core/RoleFactory.h"
#include "config/constraints.h"
#include "dclass/file/hash.h"
#include "net/TcpAcceptor.h"
#include "net/SslAcceptor.h"
using namespace std;
namespace ssl = boost::asio::ssl;

RoleConfigGroup clientagent_config("clientagent");
static ConfigVariable<string> bind_addr("bind", "0.0.0.0:7198", clientagent_config);
static ConfigVariable<string> server_version("version", "dev", clientagent_config);
static ConfigVariable<uint32_t> override_hash("manual_dc_hash", 0x0, clientagent_config);
static ValidAddressConstraint valid_bind_addr(bind_addr);

static ConfigGroup tls_config("tls", clientagent_config);
static ConfigVariable<string> tls_cert("certificate", "", tls_config);
static ConfigVariable<string> tls_key("key_file", "", tls_config);
static ConfigVariable<string> tls_chain("chain_file", "", tls_config);
static ConfigVariable<bool> sslv2_enabled("sslv2", false, tls_config);
static ConfigVariable<bool> sslv3_enabled("sslv3", false, tls_config);
static ConfigVariable<bool> tlsv1_enabled("tlsv1", true, tls_config);
static FileAvailableConstraint tls_cert_exists(tls_cert);
static FileAvailableConstraint tls_key_exists(tls_key);
static FileAvailableConstraint tls_chain_exists(tls_chain);
static BooleanValueConstraint sslv2_is_boolean(sslv2_enabled);
static BooleanValueConstraint sslv3_is_boolean(sslv3_enabled);
static BooleanValueConstraint tlsv1_is_boolean(tlsv1_enabled);

static ConfigGroup channels_config("channels", clientagent_config);
static ConfigVariable<channel_t> min_channel("min", INVALID_CHANNEL, channels_config);
static ConfigVariable<channel_t> max_channel("max", INVALID_CHANNEL, channels_config);
static InvalidChannelConstraint min_not_invalid(min_channel);
static InvalidChannelConstraint max_not_invalid(max_channel);
static ReservedChannelConstraint min_not_reserved(min_channel);
static ReservedChannelConstraint max_not_reserved(max_channel);

ConfigGroup ca_client_config("client", clientagent_config);
ConfigVariable<string> ca_client_type("type", "libastron", ca_client_config);
bool have_client_type(const string& backend)
{
	return ClientFactory::singleton().has_client_type(backend);
}
ConfigConstraint<string> client_type_exists(have_client_type, ca_client_type,
		"No Client handler exists for the given client type.");

ClientAgent::ClientAgent(RoleConfig roleconfig) : Role(roleconfig), m_net_acceptor(nullptr),
	m_server_version(server_version.get_rval(roleconfig))
{
	stringstream ss;
	ss << "Client Agent (" << bind_addr.get_rval(roleconfig) << ")";
	m_log = new LogCategory("clientagent", ss.str());

	// We need to get the client type...
	ConfigNode client = clientagent_config.get_child_node(ca_client_config, roleconfig);
	m_client_type = ca_client_type.get_rval(client);

	// ... and also the channel range ...
	ConfigNode channels = clientagent_config.get_child_node(channels_config, roleconfig);
	m_ct = ChannelTracker(min_channel.get_rval(channels), max_channel.get_rval(channels));

	// ... then store a copy of the client config.
	m_clientconfig = clientagent_config.get_child_node(ca_client_config, roleconfig);

	// Calculate the DC hash
	const uint32_t config_hash = override_hash.get_rval(roleconfig);
	if(config_hash > 0x0)
	{
		m_hash = config_hash;
	}
	else
	{
		m_hash = dclass::legacy_hash(g_dcf);
	}

	// Load SSL data from Config vars
	string certificate = tls_cert.get_rval(roleconfig);
	string key_file = tls_key.get_rval(roleconfig);

	// Handle no SSL
	if(certificate.empty() && key_file.empty())
	{
		TcpAcceptorCallback callback = std::bind(&ClientAgent::handle_tcp,
		                                         this, std::placeholders::_1);
		m_net_acceptor = new TcpAcceptor(io_service, callback);

		// TODO: Yell loudly at the user that this shouldn't be used in production
	}

	// Handle SSL requested, but some information missing
	else if(certificate.empty() != key_file.empty())
	{
		m_log->fatal() << "TLS requested but either certificate or key is missing.\n";
		exit(1);
	}

	// Handle SSL
	else
	{
		// Get the enabled SSL/TLS protocols
		long int options = ssl::context::default_workarounds;
		if(!sslv2_enabled.get_rval(roleconfig))
		{
			options |= ssl::context::no_sslv2;
		}
		if(!sslv3_enabled.get_rval(roleconfig))
		{
			options |= ssl::context::no_sslv3;
		}
		if(!tlsv1_enabled.get_rval(roleconfig))
		{
			options |= ssl::context::no_tlsv1;
		}

		// Prepare the context
		ssl::context ctx(ssl::context::sslv23);
		ctx.set_options(options);
		ctx.use_certificate_file(certificate, ssl::context::file_format::pem);
		ctx.use_private_key_file(tls_key.get_rval(roleconfig), ssl::context::file_format::pem);

		string chain_file = tls_chain.get_rval(roleconfig);
		if(!chain_file.empty()) {
			ctx.use_certificate_chain_file(chain_file);
		}

		SslAcceptorCallback callback = std::bind(&ClientAgent::handle_ssl,
		                                         this, std::placeholders::_1);
		m_net_acceptor = new SslAcceptor(io_service, ctx, callback);
	}

	// Begin listening for new Clients
	boost::system::error_code ec;
	ec = m_net_acceptor->bind(bind_addr.get_rval(m_roleconfig), 7198);
	if(ec.value() != 0)
	{
		m_log->fatal() << "Could not bind listening port: "
		               << bind_addr.get_val() << std::endl;
		m_log->fatal() << "Error code: " << ec.value()
		               << "(" << ec.category().message(ec.value()) << ")"
		               << std::endl;
		exit(1);
	}
	m_net_acceptor->start();
}

ClientAgent::~ClientAgent()
{
	delete m_log;
}

// handle_tcp generates a new Client object from a raw tcp connection.
void ClientAgent::handle_tcp(tcp::socket *socket)
{
	tcp::endpoint remote;
	try
	{
		remote = socket->remote_endpoint();
	}
	catch (exception&)
	{
		// A client might disconnect immediately after connecting.
		// If this happens, do nothing. Resolves #122.
		// N.B. due to a Boost.Asio bug, the socket will (may?) still have
		// is_open() == true, so we just catch the exception on remote_endpoint
		// instead.
		return;
	}
	m_log->debug() << "Got an incoming connection from "
	               << remote.address() << ":" << remote.port() << endl;

	ClientFactory::singleton().instantiate_client(m_client_type, m_clientconfig, this, socket);
}

// handle_ssl generates a new Client object from an ssl stream.
void ClientAgent::handle_ssl(ssl::stream<tcp::socket> *stream)
{
	tcp::endpoint remote;
	try
	{
		remote = stream->next_layer().remote_endpoint();
	}
	catch (exception&)
	{
		// A client might disconnect immediately after connecting.
		// If this happens, do nothing. Resolves #122.
		// N.B. due to a Boost.Asio bug, the socket will (may?) still have
		// is_open() == true, so we just catch the exception on remote_endpoint
		// instead.
		return;
	}
	m_log->debug() << "Got an incoming connection from "
	               << remote.address() << ":" << remote.port() << endl;

	ClientFactory::singleton().instantiate_client(m_client_type, m_clientconfig, this, stream);
}


// handle_datagram handles Datagrams received from the message director.
void ClientAgent::handle_datagram(DatagramHandle, DatagramIterator&)
{
	// At the moment, the client agent doesn't actually handle any datagrams
}

static RoleFactoryItem<ClientAgent> ca_fact("clientagent");



/* ========================== *
 *       HELPER CLASSES       *
 * ========================== */
ChannelTracker::ChannelTracker(channel_t min, channel_t max) : m_next(min), m_max(max),
	m_unused_channels()
{
}

channel_t ChannelTracker::alloc_channel()
{
	if(m_next <= m_max)
	{
		return m_next++;
	}
	else
	{
		if(!m_unused_channels.empty())
		{
			channel_t c = m_unused_channels.front();
			m_unused_channels.pop();
			return c;
		}
	}
	return 0;
}

void ChannelTracker::free_channel(channel_t channel)
{
	m_unused_channels.push(channel);
}
