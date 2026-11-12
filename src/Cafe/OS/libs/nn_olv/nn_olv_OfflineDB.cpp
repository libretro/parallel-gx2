#include "nn_olv_Common.h"
#include "nn_olv_PostTypes.h"
#include "nn_olv_OfflineDB.h"
#include "Cemu/ncrypto/ncrypto.h" // for base64 encoder/decoder
#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"
#include "util/xml/Xml.h"
#include "Cafe/Filesystem/zarchive.h"
#include "olv_offline.h"
#include "Common/FileStream.h"
#include "util/helpers/OnceGate.h"

namespace nn
{
	namespace olv
	{
		// Opened once, by the first game call that wants it (OnceGate: one at a
		// time waits for it rather than a lock), and closed with the module
		OnceGate g_offlineDBOpened;
		zarchive_t* g_offlineDBArchive{nullptr};

		void OfflineDB_LazyInit()
		{
			g_offlineDBOpened.Run([] {
				g_offlineDBArchive = zarchive_open(FileStream::HostPath(ActiveSettings::GetUserDataPath("resources/miiverse/OfflineDB.zar")).c_str(), 0, nullptr, 0);
				if (!g_offlineDBArchive)
					cemuLog_log(LogType::Force, "Offline miiverse posts are not available");
			});
		}

		// when the module is unloaded: nothing of the game's runs then
		void OfflineDB_Shutdown()
		{
			if (!g_offlineDBOpened.IsDone())
				return;
			zarchive_close(g_offlineDBArchive);
			g_offlineDBArchive = nullptr;
			g_offlineDBOpened.Reset();
		}

		// a post's files, by its id: the memo and the picture (olv_offline.c)
		static bool PostFileName(char (&name)[64], const char* dir, const DownloadedDataBase& post, const char* ext)
		{
			return olv_offline_name(name, sizeof(name), dir, (const uint8*)post.postId, sizeof(post.postId), ext);
		}

		void TryLoadCompressedMemoImage(DownloadedPostData& downloadedPostData)
		{
			auto& post = downloadedPostData.downloadedDataBase;
			char name[64];
			uint32 size = 0;
			if (!PostFileName(name, "memo/", post, "") ||
				!olv_offline_memo(g_offlineDBArchive, name, (uint8*)post.compressedMemoBody, sizeof(post.compressedMemoBody), &size))
				return;
			post.compressedMemoBodySize = size;
			post.SetFlag(DownloadedDataBase::FLAGS::HAS_BODY_MEMO);
		}

		void CheckForExternalImage(DownloadedPostData& downloadedPostData)
		{
			auto& post = downloadedPostData.downloadedDataBase;
			char name[64];
			uint32 size = 0;
			if (!PostFileName(name, "image/", post, ".jpg") || strlen(name) >= sizeof(post.externalImageDataUrl) ||
				!olv_offline_size(g_offlineDBArchive, name, &size))
				return;
			strcpy((char*)post.externalImageDataUrl, name);
			post.SetFlag(DownloadedDataBase::FLAGS::HAS_EXTERNAL_IMAGE);
			post.externalImageDataSize = size;
		}

		// Done on the calling thread, as the console's own call blocks it: the
		// archive is read from memory, where each call started a thread to do
		// it and waited for that.
		static nnResult DownloadPostDataList(DownloadedTopicData* downloadedTopicData, DownloadedPostData* downloadedPostData, uint32be* postCountOut, uint32 maxCount)
		{
			uint64 titleId = CafeSystem::GetForegroundTitleId();

			memset(downloadedTopicData, 0, sizeof(DownloadedTopicData));
			memset(downloadedPostData, 0, sizeof(DownloadedPostData) * maxCount);
			*postCountOut = 0;

			const char* postXmlFilename = nullptr;
			if(titleId == 0x0005000010143400 || titleId == 0x0005000010143500 || titleId == 0x0005000010143600)
				postXmlFilename = "PostList_WindWakerHD.xml";

			if (!postXmlFilename)
				return OLV_RESULT_SUCCESS;

			// load post XML
			uint32 xmlSize = 0;
			if (!olv_offline_size(g_offlineDBArchive, postXmlFilename, &xmlSize))
				return OLV_RESULT_SUCCESS;
			std::vector<uint8> xmlData(xmlSize);
			if (olv_offline_read(g_offlineDBArchive, postXmlFilename, xmlData.data(), xmlSize, &xmlSize) != OLV_OFFLINE_OK)
				return OLV_RESULT_SUCCESS;
			xml::Document doc;
			bool result = doc.load(xmlData.data(), xmlData.size());
			if (!result)
				return OLV_RESULT_SUCCESS;
			// collect list of all post xml nodes
			std::vector<xml::Node> postXmlNodes;
			for (xml::Node postNode = doc.child("posts").child("post"); postNode; postNode = postNode.next_sibling("post"))
				postXmlNodes.push_back(postNode);

			// randomly select up to maxCount posts
			struct Pick
			{
				std::vector<xml::Node>& nodes;
				DownloadedPostData* out;
			} pick{postXmlNodes, downloadedPostData};
			std::vector<uint32> pool(postXmlNodes.size());
			const uint32 postCount = olv_offline_pick((uint32)postXmlNodes.size(), maxCount, GetTickCount(), pool.data(),
				[](void* ud, uint32 index, uint32 slot) -> bool {
					Pick& p = *(Pick*)ud;
					auto& addedPost = p.out[slot];
					memset(&addedPost, 0, sizeof(DownloadedPostData));
					if (!ParseXML_DownloadedPostData(addedPost, p.nodes[index]))
					{
						memset(&addedPost, 0, sizeof(DownloadedPostData));
						return false;
					}
					TryLoadCompressedMemoImage(addedPost);
					CheckForExternalImage(addedPost);
					return true;
				}, &pick);
			*postCountOut = postCount;
			return OLV_RESULT_SUCCESS;
		}

		nnResult OfflineDB_DownloadPostDataListParam_DownloadPostDataList(DownloadedTopicData* downloadedTopicData, DownloadedPostData* downloadedPostData, uint32be* postCountOut, uint32 maxCount, DownloadPostDataListParam* param)
		{
			OfflineDB_LazyInit();

			memset(downloadedTopicData, 0, sizeof(DownloadedTopicData));
			downloadedTopicData->communityId = param->communityId;
			*postCountOut = 0;

			if(param->_HasFlag(DownloadPostDataListParam::FLAGS::SELF_ONLY))
				return OLV_RESULT_SUCCESS; // the offlineDB doesn't contain any self posts

			return DownloadPostDataList(downloadedTopicData, downloadedPostData, postCountOut, maxCount);
		}

		// The picture into imageDataOut, maxSize octets of room: one that does
		// not fit is refused, where it was copied whole past the end of it
		nnResult OfflineDB_DownloadPostDataListParam_DownloadExternalImageData(DownloadedDataBase* _this, void* imageDataOut, uint32be* imageSizeOut, uint32 maxSize)
		{
			if (!_this->TestFlags(_this, DownloadedDataBase::FLAGS::HAS_EXTERNAL_IMAGE))
				return OLV_RESULT_MISSING_DATA;
			// not all games may use JPEG files?
			char name[64];
			if (!PostFileName(name, "image/", *_this, ".jpg"))
				return OLV_RESULT_FAILED_REQUEST;
			uint32 size = 0;
			switch (olv_offline_read(g_offlineDBArchive, name, imageDataOut, maxSize, &size))
			{
			case OLV_OFFLINE_OK:
				*imageSizeOut = size;
				return OLV_RESULT_SUCCESS;
			case OLV_OFFLINE_TOO_LARGE:
				return OLV_RESULT_NOT_ENOUGH_SIZE;
			default:
				return OLV_RESULT_FAILED_REQUEST;
			}
		}

	}
}