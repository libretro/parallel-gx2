#include "nn_olv_Common.h"
#include "nn_olv_PostTypes.h"
#include "nn_olv_OfflineDB.h"
#include "Cemu/ncrypto/ncrypto.h" // for base64 encoder/decoder
#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"
#include "util/xml/Xml.h"
#include "util/helpers/Deflate.h"
#include "Cafe/Filesystem/zarchive.h"
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

		static bool LookUpOfflineDBFile(const char* filePath, zar_file_t& file)
		{
			return g_offlineDBArchive && zarchive_lookup(g_offlineDBArchive, filePath, strlen(filePath), &file) && !file.dir;
		}

		bool CheckForOfflineDBFile(const char* filePath, uint32* fileSize)
		{
			zar_file_t file;
			if (!LookUpOfflineDBFile(filePath, file))
				return false;
			if(fileSize)
				*fileSize = (uint32)file.size;
			return true;
		}

		bool LoadOfflineDBFile(const char* filePath, std::vector<uint8>& fileData)
		{
			fileData.clear();
			zar_file_t file;
			if (!LookUpOfflineDBFile(filePath, file))
				return false;
			fileData.resize(file.size);
			return zarchive_read(g_offlineDBArchive, &file, 0, fileData.data(), fileData.size()) == fileData.size();
		}

		void TryLoadCompressedMemoImage(DownloadedPostData& downloadedPostData)
		{
			const unsigned char tgaHeader_320x120_32BPP[] = {0x0,0x0,0x2,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x40,0x1,0x78,0x0,0x20,0x8};
			std::string memoImageFilename = fmt::format("memo/{}", (char*)downloadedPostData.downloadedDataBase.postId);
			std::vector<uint8> bitmaskCompressedImg;
			if (!LoadOfflineDBFile(memoImageFilename.c_str(), bitmaskCompressedImg))
				return;
			if (bitmaskCompressedImg.size() != (320*120)/8)
				return;
			std::vector<uint8> decompressedImage;
			decompressedImage.resize(sizeof(tgaHeader_320x120_32BPP) + 320 * 120 * 4);
			memcpy(decompressedImage.data(), tgaHeader_320x120_32BPP, sizeof(tgaHeader_320x120_32BPP));
			uint8* pOut = decompressedImage.data() + sizeof(tgaHeader_320x120_32BPP);
			for(int i=0; i<320*120; i++)
			{
				bool isWhite = (bitmaskCompressedImg[i/8] & (1 << (i%8))) != 0;
				if(isWhite)
				{
					pOut[0] = pOut[1] = pOut[2] = pOut[3] = 0xFF;
				}
				else
				{
					pOut[0] = pOut[1] = pOut[2] = 0;
					pOut[3] = 0xFF;
				}
				pOut += 4;
			}
			// store compressed image
			size_t compressedDestLen = 40960;
			if (!Deflate::Compress((uint8*)downloadedPostData.downloadedDataBase.compressedMemoBody, compressedDestLen, decompressedImage.data(), decompressedImage.size()))
				return;
			downloadedPostData.downloadedDataBase.compressedMemoBodySize = compressedDestLen;
			downloadedPostData.downloadedDataBase.SetFlag(DownloadedDataBase::FLAGS::HAS_BODY_MEMO);
		}

		void CheckForExternalImage(DownloadedPostData& downloadedPostData)
		{
			std::string externalImageFilename = fmt::format("image/{}.jpg", (char*)downloadedPostData.downloadedDataBase.postId);
			uint32 fileSize;
			if (!CheckForOfflineDBFile(externalImageFilename.c_str(), &fileSize))
				return;
			strcpy((char*)downloadedPostData.downloadedDataBase.externalImageDataUrl, externalImageFilename.c_str());
			downloadedPostData.downloadedDataBase.SetFlag(DownloadedDataBase::FLAGS::HAS_EXTERNAL_IMAGE);
			downloadedPostData.downloadedDataBase.externalImageDataSize = fileSize;
		}

		nnResult _Async_OfflineDB_DownloadPostDataListParam_DownloadPostDataList(coreinit::OSEvent* event, DownloadedTopicData* downloadedTopicData, DownloadedPostData* downloadedPostData, uint32be* postCountOut, uint32 maxCount, DownloadPostDataListParam* param)
		{
			stdx::scope_exit _se([&](){coreinit::OSSignalEvent(event);});

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
			std::vector<uint8> xmlData;
			if (!LoadOfflineDBFile(postXmlFilename, xmlData))
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
			srand(GetTickCount());
			uint32 postCount = 0;
			while(!postXmlNodes.empty() && postCount < maxCount)
			{
				uint32 index = rand() % postXmlNodes.size();
				xml::Node& postNode = postXmlNodes[index];

				auto& addedPost = downloadedPostData[postCount];
				memset(&addedPost, 0, sizeof(DownloadedPostData));
				if (!ParseXML_DownloadedPostData(addedPost, postNode) )
					continue;
				TryLoadCompressedMemoImage(addedPost);
				CheckForExternalImage(addedPost);
				postCount++;
				// remove from post list
				postXmlNodes[index] = postXmlNodes.back();
				postXmlNodes.pop_back();
			}
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

			StackAllocator<coreinit::OSEvent> doneEvent;
			coreinit::OSInitEvent(&doneEvent, coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_MANUAL);
			auto asyncTask = std::async(std::launch::async, _Async_OfflineDB_DownloadPostDataListParam_DownloadPostDataList, doneEvent.GetPointer(), downloadedTopicData, downloadedPostData, postCountOut, maxCount, param);
			coreinit::OSWaitEvent(&doneEvent);
			nnResult r = asyncTask.get();
			return r;
		}

		nnResult _Async_OfflineDB_DownloadPostDataListParam_DownloadExternalImageData(coreinit::OSEvent* event, DownloadedDataBase* _this, void* imageDataOut, uint32be* imageSizeOut, uint32 maxSize)
		{
			stdx::scope_exit _se([&](){coreinit::OSSignalEvent(event);});

			if (!_this->TestFlags(_this, DownloadedDataBase::FLAGS::HAS_EXTERNAL_IMAGE))
				return OLV_RESULT_MISSING_DATA;

			// not all games may use JPEG files?
			std::string externalImageFilename = fmt::format("image/{}.jpg", (char*)_this->postId);
			std::vector<uint8> jpegData;
			if (!LoadOfflineDBFile(externalImageFilename.c_str(), jpegData))
				return OLV_RESULT_FAILED_REQUEST;

			memcpy(imageDataOut, jpegData.data(), jpegData.size());
			*imageSizeOut = jpegData.size();

			return OLV_RESULT_SUCCESS;
		}

		nnResult OfflineDB_DownloadPostDataListParam_DownloadExternalImageData(DownloadedDataBase* _this, void* imageDataOut, uint32be* imageSizeOut, uint32 maxSize)
		{
			StackAllocator<coreinit::OSEvent> doneEvent;
			coreinit::OSInitEvent(&doneEvent, coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_MANUAL);
			auto asyncTask = std::async(std::launch::async, _Async_OfflineDB_DownloadPostDataListParam_DownloadExternalImageData, doneEvent.GetPointer(), _this, imageDataOut, imageSizeOut, maxSize);
			coreinit::OSWaitEvent(&doneEvent);
			nnResult r = asyncTask.get();
			return r;
		}

	}
}