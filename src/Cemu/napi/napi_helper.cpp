#include "Common/precompiled.h"
#include "napi.h"

#include "Common/http_xfer.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"

#include "Cemu/ncrypto/ncrypto.h"
#include "napi_helper.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "config/ActiveSettings.h"
#include "config/NetworkSettings.h"
#include "config/LaunchSettings.h"
#include "util/xml/Xml.h"
#include <charconv>

namespace
{
	void AddCA(http_xfer_tls* tls, sint32 id)
	{
		if (!iosuCrypto_addCACertificate(tls, id))
			cemuLog_log(LogType::Force, "Invalid CA certificate ({})", id);
	}

	void AddClient(http_xfer_tls* tls, sint32 id)
	{
		if (!iosuCrypto_addClientCertificate(tls, id))
			cemuLog_log(LogType::Force, "Invalid client certificate ({})", id);
	}

	// "Name: value" headers as net_http takes them: Content-Type and
	// User-Agent in their own slots, Host and Content-Length its own
	void SplitHeaders(const std::vector<std::string>& fields, std::string& headers, std::string& contentType, std::string& userAgent)
	{
		for (const std::string& f : fields)
		{
			const size_t colon = f.find(':');
			if (colon == std::string::npos)
				continue;
			std::string_view name(f.data(), colon);
			size_t v = colon + 1;
			while (v < f.size() && (f[v] == ' ' || f[v] == '\t'))
				v++;
			if (boost::iequals(name, "Content-Type"))
				contentType = f.substr(v);
			else if (boost::iequals(name, "User-Agent"))
				userAgent = f.substr(v);
			else if (!boost::iequals(name, "Content-Length") && !boost::iequals(name, "Host"))
			{
				headers += f;
				headers += "\r\n";
			}
		}
	}

	// the request, its TLS set up, sent once - and, with retry, once more if
	// it fails, received (when not handed on) anew
	int Perform(http_xfer_request_t& req, NetworkService service, HttpRequestHelper::SERVER_SSL_CONTEXT ctx, const HttpRequestHelper* custom,
		bool (*onBody)(void*, const void*, size_t), void* bodyCtx, std::vector<uint8>* received, bool retry, int& status)
	{
		int res = HTTP_XFER_OK;
		for (int attempt = 0; attempt < (retry ? 2 : 1); attempt++)
		{
			if (received)
				received->clear();
			req.tls = nullptr;
			if (boost::istarts_with(req.url, "https:"))
			{
				req.tls = http_xfer_tls_new();
				if (IsNetworkServiceSSLDisabled(service))
					http_xfer_tls_set_verify(req.tls, false, false);
				else
					HttpRequestHelper::SetupTLS(req.tls, ctx, custom);
			}
			res = http_xfer_perform(&req, onBody, bodyCtx, &status); // takes req.tls
			if (res == HTTP_XFER_OK || !retry || attempt == 1)
				break;
			cemuLog_log(LogType::Force, "Web request failed with error {}. Retrying...", res);
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		}
		return res;
	}
}

void HttpRequestHelper::SetupTLS(http_xfer_tls* tls, SERVER_SSL_CONTEXT ctx, const HttpRequestHelper* custom)
{
	switch (ctx)
	{
	case SERVER_SSL_CONTEXT::ACT:
	case SERVER_SSL_CONTEXT::TAGAYA:
		AddCA(tls, 102);
		AddCA(tls, 105);
		AddClient(tls, 3);
		break;
	case SERVER_SSL_CONTEXT::IDBE:
		AddCA(tls, 105);
		break;
	case SERVER_SSL_CONTEXT::IAS:
	case SERVER_SSL_CONTEXT::ECS:
	case SERVER_SSL_CONTEXT::CCS:
		AddCA(tls, 102);
		AddClient(tls, 1);
		break;
	case SERVER_SSL_CONTEXT::OLIVE:
	{
		AddCA(tls, 105);
		AddClient(tls, 7);
		static const sint16 certGroups[] = {
			100,  101,  102,   103,  104,  105,
			1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009,
			1010, 1011, 1012, 1013, 1014, 1015, 1016, 1017, 1018, 1019,
			1020, 1021, 1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029,
			1030, 1031, 1032, 1033
		};
		for (sint16 certId : certGroups)
			iosuCrypto_addCACertificate(tls, certId);
		break;
	}
	case SERVER_SSL_CONTEXT::CUSTOM:
		if (!custom)
			break;
		for (sint32 caCertId : custom->GetCaCertIds())
			AddCA(tls, caCertId);
		for (sint32 clientCertId : custom->GetClientCertIds())
			AddClient(tls, clientCertId);
		if (custom->GetClientCertIds().empty())
			http_xfer_tls_set_verify(tls, false, false);
		break;
	default:
		cemu_assert(false);
	}
}

HttpRequestHelper::HttpRequestHelper() = default;
HttpRequestHelper::~HttpRequestHelper() = default;

void HttpRequestHelper::initate(NetworkService service, std::string url, SERVER_SSL_CONTEXT sslContext)
{
	// reset parameters
	m_headerExtraFields.clear();
	m_postData.clear();
	m_formFields.clear();
	m_cbWriteCallback = nullptr;
	m_url = std::move(url);
	m_service = service;
	m_sslContext = sslContext;
	m_timeoutSeconds = 60;
}

void HttpRequestHelper::addHeaderField(const char* fieldName, std::string_view value)
{
	m_headerExtraFields.emplace_back(fieldName, value);
}

void HttpRequestHelper::addPostField(const char* fieldName, std::string_view value)
{
	if (!m_postData.empty())
		m_postData.emplace_back('&');
	m_postData.insert(m_postData.end(), (uint8*)fieldName, (uint8*)fieldName + strlen(fieldName));
	m_postData.emplace_back('=');
	m_postData.insert(m_postData.end(), (uint8*)value.data(), (uint8*)value.data() + value.size());
}

void HttpRequestHelper::addFormField(const char* fieldName, std::string_view value)
{
	m_formFields.emplace_back(fieldName, std::string(value));
}

void HttpRequestHelper::setWriteCallback(bool(*cbWriteCallback)(void* userData, const void* ptr, size_t len, bool isLast), void* userData)
{
	m_cbWriteCallback = cbWriteCallback;
	m_writeCallbackUserData = userData;
}

void HttpRequestHelper::setTimeout(sint32 time)
{
	m_timeoutSeconds = time;
}

void HttpRequestHelper::setUserAgent(std::string_view userAgent)
{
	m_userAgent = userAgent;
}

bool HttpRequestHelper::OnBody(void* ctx, const void* data, size_t len)
{
	HttpRequestHelper* helper = (HttpRequestHelper*)ctx;
	if (helper->m_cbWriteCallback)
		return helper->m_cbWriteCallback(helper->m_writeCallbackUserData, data, len, false);
	helper->m_receiveBuffer.insert(helper->m_receiveBuffer.end(), (const uint8*)data, (const uint8*)data + len);
	return true;
}

bool HttpRequestHelper::submitRequest(bool isPost)
{
	std::vector<std::string> fields;
	for (auto& itr : m_headerExtraFields)
		fields.emplace_back(itr.data);
	std::string headers, contentType, userAgent = m_userAgent;
	SplitHeaders(fields, headers, contentType, userAgent);

	http_xfer_request_t req{};
	req.url = m_url.c_str();
	req.method = isPost ? "POST" : "GET";
	req.headers = headers.empty() ? nullptr : headers.c_str();
	req.user_agent = userAgent.empty() ? nullptr : userAgent.c_str();
	req.timeout_ms = m_timeoutSeconds > 0 ? (unsigned)m_timeoutSeconds * 1000 : 0;
	void* form = nullptr;
	char* formType = nullptr;
	if (isPost)
	{
		req.has_body = true;
		if (!m_formFields.empty())
		{
			std::vector<http_xfer_part_t> parts(m_formFields.size());
			for (size_t i = 0; i < parts.size(); i++)
			{
				parts[i].name = m_formFields[i].first.c_str();
				parts[i].data = m_formFields[i].second.data();
				parts[i].len = m_formFields[i].second.size();
				parts[i].content_type = nullptr;
			}
			form = http_xfer_multipart(parts.data(), parts.size(), &req.body_len, &formType);
			if (!form)
				return false;
			req.body = form;
			req.content_type = formType;
		}
		else
		{
			req.body = m_postData.data();
			req.body_len = m_postData.size();
		}
	}
	if (!contentType.empty())
		req.content_type = contentType.c_str();

	int status = 0;
	const int res = Perform(req, m_service, m_sslContext, this, OnBody, this, m_cbWriteCallback ? nullptr : &m_receiveBuffer, true, status);
	free(form);
	free(formType);
	m_httpStatusCode = status;
	if (res != HTTP_XFER_OK)
		return false;

	// check response code
	if (status != 200)
	{
		cemuLog_log(LogType::Force, "HTTP request received response {} but expected 200", status);
		cemuLog_log(LogType::Force, "Request: {}", m_url);
		// error status codes (4xx or 5xx range) are always considered a failed request, except for code 400 which is usually returned as a response to failed logins etc.
		if (status >= 400 && status <= 599 && status != 400)
			return false;
		// for other status codes we assume success if the message is non-empty
		if (m_receiveBuffer.empty())
			return false;
	}

	if (m_cbWriteCallback)
		m_cbWriteCallback(m_writeCallbackUserData, nullptr, 0, true); // flush write

	return true;
}

HttpSOAPHelper::HttpSOAPHelper(NetworkService service) : m_service(service)
{
}

void HttpSOAPHelper::SOAP_initate(std::string_view serviceType, std::string url, std::string_view requestMethod, std::string_view requestVersion)
{
	m_url = std::move(url);
	m_serviceType = serviceType;
	m_requestMethod = requestMethod;
	m_requestVersion = requestVersion;

	m_envelopeExtraParam.reserve(512);
	m_envelopeExtraParam.clear();
}

void HttpSOAPHelper::SOAP_addRequestField(const char* fieldName, std::string_view value)
{
	m_envelopeExtraParam.append(fmt::format("<{}:{}>{}</{}:{}>", m_serviceType, fieldName, value, m_serviceType, fieldName));
}

void HttpSOAPHelper::SOAP_generateEnvelope()
{
	m_envelopeStr.reserve(4096);
	m_envelopeStr.clear();

	m_envelopeStr.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");

	m_envelopeStr.append("<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\"\n");
	m_envelopeStr.append(" xmlns:SOAP-ENC=\"http://schemas.xmlsoap.org/soap/encoding/\"\n");
	m_envelopeStr.append(" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n");
	m_envelopeStr.append(" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"\n");
	m_envelopeStr.append(fmt::format(" xmlns:{}=\"urn:{}.wsapi.broadon.com\">\n", m_serviceType, m_serviceType));
	m_envelopeStr.append("<SOAP-ENV:Body>\n");
	m_envelopeStr.append(fmt::format("<{}:{} xsi:type=\"{}:{}RequestType\">\n", m_serviceType, m_requestMethod, m_serviceType, m_requestMethod));
	m_envelopeStr.append(fmt::format("<{}:Version>{}</{}:Version>\n", m_serviceType, m_requestVersion, m_serviceType));

	// the server echos the message id
	static uint64 s_msgHigh = 1 + (uint64)HighResolutionTimer::now().getTick()/7;
	uint64 msgId_high = s_msgHigh; // usually this is set to the deviceId
	uint64 msgId_low = (uint64)HighResolutionTimer::now().getTick(); // uptime
	m_envelopeStr.append(fmt::format("<{}:MessageId>EC-{}-{}</{}:MessageId>", m_serviceType, msgId_high, msgId_low, m_serviceType));

	m_envelopeStr.append(m_envelopeExtraParam);

	// some fields are specific to services?
	// ECS doesnt seem to like RegionId and CountryCode

	// following fields are shared:
	// >>> Region, Country, Language
	// following fields are present when NUS:
	// >>> RegionId (instead of Region), CountryCode (instead of Country)
	// following fields are present when CAS or ECS:
	// >>> ApplicationId, TIN, SerialNo
	// following fields are present when CAS:
	// >>> Age
	// following fields are present when ECS:
	// >>> SessionHandle, ServiceTicket, ServiceId
	// following fields for anything that isn't BGS:
	// >>> DeviceId (the serial)

	// DeviceId -> All except BGS (deviceId is the console serial)
	// DeviceToken -> Everything except BGS and NUS

	// ECS:
	//m_envelopeStr.append(fmt::format("<{}:Region>EUR</{}:Region>", serviceType, serviceType));
	//m_envelopeStr.append(fmt::format("<{}:Country>AT</{}:Country>", serviceType, serviceType));


	// device token format:
	// <ECS:DeviceToken>WT-<md5hash_in_hex></ECS:DeviceToken>

	// unknown fields:
	// VirtualDeviceType (shared but optional?)


	// device cert not needed for ECS:GetAccountStatus ? (it complains if present)
	//char deviceCertStr[1024 * 4];
	//iosuCrypto_getDeviceCertificateBase64Encoded(deviceCertStr);
	//m_envelopeStr.append(fmt::format("<{}:DeviceCert>{}</{}:DeviceCert>", serviceType, deviceCertStr, serviceType));

	// only device token needed
	// DeviceToken comes from GetRegistrationInfo and is then stored in ec_account_info.exi

	m_envelopeStr.append(fmt::format("</{}:{}>\n", m_serviceType, m_requestMethod));

	m_envelopeStr.append("</SOAP-ENV:Body>\n");
	m_envelopeStr.append("</SOAP-ENV:Envelope>\n");


}

sint32 iosuCrypto_getDeviceCertificateBase64Encoded(char* output);

static bool SOAPOnBody(void* ctx, const void* data, size_t len)
{
	std::vector<uint8>* buffer = (std::vector<uint8>*)ctx;
	buffer->insert(buffer->end(), (const uint8*)data, (const uint8*)data + len);
	return true;
}

bool HttpSOAPHelper::submitRequest()
{
	// generate and set envelope
	SOAP_generateEnvelope();
	std::string headers = "Accept-Charset: UTF-8\r\n";
	headers += fmt::format("SOAPAction: urn:{}.wsapi.broadon.com/{}\r\n", m_serviceType, m_requestMethod);
	headers += "Accept: */*\r\n";
	http_xfer_request_t req{};
	req.url = m_url.c_str();
	req.method = "POST";
	req.headers = headers.c_str();
	req.content_type = "text/xml; charset=utf-8";
	req.user_agent = "EVL NUP 040800 Sep 18 2012 20:20:02";
	req.body = m_envelopeStr.data();
	req.body_len = m_envelopeStr.size();
	req.has_body = true;
	int status = 0;
	return Perform(req, m_service, HttpRequestHelper::SERVER_SSL_CONTEXT::IAS, nullptr, SOAPOnBody, &m_receiveBuffer, &m_receiveBuffer, false, status) == HTTP_XFER_OK;
}

/* helper functions */

namespace NAPI
{
	bool _findXmlNode(xml::Node& doc, xml::Node& nodeOut, const char* name)
	{
		for (auto itr : doc.children())
		{
			if (boost::iequals(itr.name(), name))
			{
				nodeOut = itr;
				return true;
			}
			if (_findXmlNode(itr, nodeOut, name))
				return true;
		}
		return false;
	}

	bool _parseResponseInit(const HttpSOAPHelper& soapHelper, const char* responseNodeName, xml::Node& node, _NAPI_CommonResultSOAP& result, xml::Document& doc, xml::Node& responseNode)
	{
		// parse XML response
		if (!doc.load(soapHelper.getReceivedData().data(), soapHelper.getReceivedData().size()))
		{
			cemuLog_log(LogType::Force, "Failed to parse GetRegistrationInfo() response");
			result.apiError = NAPI_RESULT::XML_ERROR;
			return false;
		}
		if (!_findXmlNode(doc, node, responseNodeName))
		{
			result.apiError = NAPI_RESULT::XML_ERROR;
			return false;
		}
		// parse error code
		auto errorCodeStr = node.child_value("ErrorCode");
		if (!errorCodeStr)
		{
			result.apiError = NAPI_RESULT::XML_ERROR;
			return false;
		}
		int parsedErrorCode = 0;
		std::from_chars_result fcr = std::from_chars(errorCodeStr, errorCodeStr + strlen(errorCodeStr), parsedErrorCode);
		if (fcr.ec == std::errc::invalid_argument || fcr.ec == std::errc::result_out_of_range)
		{
			result.apiError = NAPI_RESULT::XML_ERROR;
			return false;
		}
		if (parsedErrorCode != 0)
		{
			result.serviceError = (EC_ERROR_CODE)parsedErrorCode;
			result.apiError = NAPI_RESULT::SERVICE_ERROR;
			return false;
		}
		result.apiError = NAPI_RESULT::SUCCESS;
		return true;
	}
};
