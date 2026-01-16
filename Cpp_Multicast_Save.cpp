/***************************************************************************************
 ***                                                                                 ***
 ***  Copyright (c) 2025, Lucid Vision Labs, Inc.                                    ***
 ***                                                                                 ***
 ***  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR     ***
 ***  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,       ***
 ***  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE    ***
 ***  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER         ***
 ***  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,  ***
 ***  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE  ***
 ***  SOFTWARE.                                                                      ***
 ***                                                                                 ***
 ***************************************************************************************/

#include "stdafx.h"
#include "ArenaApi.h"
#include "SaveApi.h"
#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstring>
#include <deque>
#include <errno.h>
#include <fcntl.h>
#include <iomanip>
#include <limits.h>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <thread>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TAB1 "  "
#define TAB2 "    "

// Multicast
//    This example demonstrates multicasting from the master's perspective.
//    Multicasting allows for the streaming of images and events to multiple
//    destinations. Multicasting requires nearly the same steps for both masters
//    and listeners. The only difference, as seen below, is that device features
//    can only be set by the master.

// =-=-=-=-=-=-=-=-=-
// =-=- SETTINGS =-=-
// =-=-=-=-=-=-=-=-=-

// image timeout
#define TIMEOUT 2000

// pixel format
#define PIXEL_FORMAT BGR8

// multicast group IP (fixed)
#define MULTICAST_GROUP_IP "239.10.10.10"

// Length of time to grab images (sec)
//    Note that the listener must be started while the master is still streaming,
//    and that the listener will not receive any more images once the master
//    stops streaming.
//    Retained from the original example; loop now exits on ESC.
#define NUM_SECONDS 20

// =-=-=-=-=-=-=-=-=-=-=-=-=-
// =- OUTPUT DIRECTORY HELPER
// =-=-=-=-=-=-=-=-=-=-=-=-=-

// Resolve the directory of the running executable (Linux /proc/self/exe).
static std::string GetExecutableDir()
{
	char exePath[PATH_MAX];
	ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
	if (len == -1)
	{
		throw std::runtime_error(std::string("Failed to resolve executable path: ") + std::strerror(errno));
	}
	exePath[len] = '\0';

	std::string fullPath(exePath);
	size_t lastSlash = fullPath.find_last_of('/');
	if (lastSlash == std::string::npos)
		return ".";

	return fullPath.substr(0, lastSlash);
}

// Format program start time for folder naming.
static std::string GetRunTimestamp()
{
	std::time_t now = std::time(NULL);
	std::tm localTime;
	localtime_r(&now, &localTime);

	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &localTime);
	return std::string(buffer);
}

// Create directories recursively for a path.
static bool EnsureDir(const std::string& path)
{
	if (path.empty())
		return false;

	std::string current;
	if (path[0] == '/')
		current = "/";

	size_t start = (path[0] == '/') ? 1 : 0;
	for (size_t i = start; i <= path.size(); ++i)
	{
		if (i == path.size() || path[i] == '/')
		{
			std::string part = path.substr(start, i - start);
			if (!part.empty())
			{
				if (current.size() > 1 && current[current.size() - 1] != '/')
					current += "/";
				current += part;

				if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST)
					return false;
			}
			start = i + 1;
		}
	}

	return true;
}

// Create the output directory under the executable directory.
static std::string CreateOutputDir()
{
	std::string outputDir = GetExecutableDir() + "/imgs/" + GetRunTimestamp();
	if (!EnsureDir(outputDir))
	{
		std::ostringstream message;
		message << "Failed to create output directory: " << outputDir << " (" << std::strerror(errno) << ")";
		throw std::runtime_error(message.str());
	}

	return outputDir;
}

// =-=-=-=-=-=-=-=-=-=-=-=-=-=-
// =- ASYNC SAVE QUEUE HELPERS
// =-=-=-=-=-=-=-=-=-=-=-=-=-=-

struct SaveJob
{
	// Image copy to be saved and destroyed by the worker.
	Arena::IImage* pImage;
	std::string filename;
};

struct SaveQueue
{
	// Single-producer/single-consumer queue for disk writes.
	std::deque<SaveJob> jobs;
	std::mutex mutex;
	std::condition_variable cv;
	bool stop;
};

void SaveImage(Arena::IImage* pImage, const char* filename);

static void SaveWorker(SaveQueue* queue)
{
	// Drain save jobs on a background thread to avoid blocking acquisition.
	for (;;)
	{
		SaveJob job;
		{
			std::unique_lock<std::mutex> lock(queue->mutex);
			queue->cv.wait(lock, [&]() { return queue->stop || !queue->jobs.empty(); });
			if (queue->stop && queue->jobs.empty())
				break;

			job = queue->jobs.front();
			queue->jobs.pop_front();
		}

		try
		{
			SaveImage(job.pImage, job.filename.c_str());
		}
		catch (GenICam::GenericException& ge)
		{
			std::cout << "\nGenICam exception thrown while saving: " << ge.what() << "\n";
		}
		catch (std::exception& ex)
		{
			std::cout << "\nStandard exception thrown while saving: " << ex.what() << "\n";
		}
		catch (...)
		{
			std::cout << "\nUnexpected exception thrown while saving\n";
		}

		Arena::ImageFactory::Destroy(job.pImage);
	}
}

static void EnqueueSave(SaveQueue* queue, SaveJob job)
{
	{
		std::lock_guard<std::mutex> lock(queue->mutex);
		queue->jobs.push_back(job);
	}
	queue->cv.notify_one();
}

static void StopSaveWorker(SaveQueue* queue, std::thread& worker)
{
	// Signal the worker to flush and exit.
	{
		std::lock_guard<std::mutex> lock(queue->mutex);
		queue->stop = true;
	}
	queue->cv.notify_all();
	if (worker.joinable())
		worker.join();
}

struct SaveWorkerGuard
{
	SaveQueue* queue;
	std::thread* worker;
	~SaveWorkerGuard()
	{
		// Ensure pending saves are flushed before returning.
		if (worker && worker->joinable())
			StopSaveWorker(queue, *worker);
	}
};

struct TerminalSettings
{
	// Terminal state for non-blocking ESC detection.
	bool enabled;
	termios originalTermios;
	int originalFlags;
};

static TerminalSettings SetupTerminalForEsc()
{
	// Put stdin into non-canonical, non-echo, non-blocking mode.
	TerminalSettings settings = {};
	settings.enabled = false;
	settings.originalFlags = -1;

	if (!isatty(STDIN_FILENO))
		return settings;

	if (tcgetattr(STDIN_FILENO, &settings.originalTermios) != 0)
		return settings;

	termios raw = settings.originalTermios;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
		return settings;

	settings.originalFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
	if (settings.originalFlags != -1)
		fcntl(STDIN_FILENO, F_SETFL, settings.originalFlags | O_NONBLOCK);

	settings.enabled = true;
	return settings;
}

static void RestoreTerminal(const TerminalSettings& settings)
{
	// Restore terminal settings if they were changed.
	if (!settings.enabled)
		return;

	tcsetattr(STDIN_FILENO, TCSANOW, &settings.originalTermios);
	if (settings.originalFlags != -1)
		fcntl(STDIN_FILENO, F_SETFL, settings.originalFlags);
}

struct TerminalGuard
{
	// RAII restore for terminal settings.
	TerminalSettings settings;
	~TerminalGuard()
	{
		RestoreTerminal(settings);
	}
};

static bool CheckForEsc(const TerminalSettings& settings)
{
	// Consume all pending input; return true if ESC (27) is seen.
	if (!settings.enabled)
		return false;

	char ch = 0;
	ssize_t bytesRead = read(STDIN_FILENO, &ch, 1);
	while (bytesRead > 0)
	{
		if (ch == 27)
			return true;
		bytesRead = read(STDIN_FILENO, &ch, 1);
	}
	return false;
}

struct MulticastGuard
{
	int socketFd;
	ip_mreqn request;
	bool joined;

	MulticastGuard()
		: socketFd(-1)
		, joined(false)
	{
		std::memset(&request, 0, sizeof(request));
	}

	~MulticastGuard()
	{
		if (joined)
			setsockopt(socketFd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &request, sizeof(request));
		if (socketFd != -1)
			close(socketFd);
	}

	void Join(const char* interfaceName)
	{
		socketFd = socket(AF_INET, SOCK_DGRAM, 0);
		if (socketFd < 0)
			throw std::runtime_error(std::string("Failed to create socket: ") + std::strerror(errno));

		unsigned int ifIndex = if_nametoindex(interfaceName);
		if (ifIndex == 0)
			throw std::runtime_error(std::string("Invalid interface name: ") + interfaceName);

		if (inet_pton(AF_INET, MULTICAST_GROUP_IP, &request.imr_multiaddr) != 1)
			throw std::runtime_error("Invalid multicast group IP");

		request.imr_ifindex = static_cast<int>(ifIndex);
		request.imr_address.s_addr = htonl(INADDR_ANY);

		if (setsockopt(socketFd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof(request)) != 0)
			throw std::runtime_error(std::string("Failed to join multicast group: ") + std::strerror(errno));

		joined = true;
	}
};

// =-=-=-=-=-=-=-=-=-
// =-=- EXAMPLE -=-=-
// =-=-=-=-=-=-=-=-=- 
// (1) enable multicast
// (2) prepare settings on master, not on listener
// (3) stream regularly

// demonstrates saving an image
// (1) converts image to a displayable pixel format
// (2) prepares image parameters
// (3) prepares image writer
// (4) saves image
// (5) destroys converted image
void SaveImage(Arena::IImage* pImage, const char* filename)
{

	// Convert image
	//    Convert the image to a displayable pixel format. It is worth keeping in
	//    mind the best pixel and file formats for your application. This example
	//    converts the image so that it is displayable by the operating system.
	std::cout << TAB1 << "Convert image to " << GetPixelFormatName(PIXEL_FORMAT) << "\n";

	auto pConverted = Arena::ImageFactory::Convert(
		pImage,
		PIXEL_FORMAT);

	// Prepare image parameters
	//    An image's width, height, and bits per pixel are required to save to
	//    disk. Its size and stride (i.e. pitch) can be calculated from those 3
	//    inputs. Notice that an image's size and stride use bytes as a unit
	//    while the bits per pixel uses bits.
	std::cout << TAB1 << "Prepare image parameters\n";

	Save::ImageParams params(
		pConverted->GetWidth(),
		pConverted->GetHeight(),
		pConverted->GetBitsPerPixel());

	// Prepare image writer
	//    The image writer requires 3 arguments to save an image: the image's
	//    parameters, a specified file name or pattern, and the image data to
	//    save. Providing these should result in a successfully saved file on the
	//    disk. Because an image's parameters and file name pattern may repeat,
	//    they can be passed into the image writer's constructor.
	std::cout << TAB1 << "Prepare image writer\n";

	Save::ImageWriter writer(
		params,
		filename);

	// Save image
	//    Passing image data into the image writer using the cascading I/O
	//    operator (<<) triggers a save. Notice that the << operator accepts the
	//    image data as a constant unsigned 8-bit integer pointer (const
	//    uint8_t*) and the file name as a character string (const char*).
	std::cout << TAB1 << "Save image\n";

	writer << pConverted->GetData();

	// destroy converted image
	Arena::ImageFactory::Destroy(pConverted);
}

void AcquireImages(Arena::IDevice* pDevice, const std::string& outputDir)
{
	// get node values that will be changed in order to return their values at
	// the end of the example
	GenICam::gcstring acquisitionModeInitial = Arena::GetNodeValue<GenICam::gcstring>(pDevice->GetNodeMap(), "AcquisitionMode");

	// Enable multicast
	//    Multicast must be enabled on both the master and listener. A small
	//    number of transport layer features will remain writable even though a
	//    device's access mode might be read-only.
	std::cout << TAB1 << "Enable multicast\n";

	Arena::SetNodeValue<bool>(
		pDevice->GetTLStreamNodeMap(),
		"StreamMulticastEnable",
		true);

	// Prepare settings on master, not on listener
	//    Device features must be set on the master rather than the listener.
	//    This is because the listener is opened with a read-only access mode.
	GenICam::gcstring deviceAccessStatus = Arena::GetNodeValue<GenICam::gcstring>(
		pDevice->GetTLDeviceNodeMap(),
		"DeviceAccessStatus");

	// master
	if (deviceAccessStatus == "ReadWrite")
	{
		std::cout << TAB1 << "Host streaming as 'master'\n";

		// set acquisition mode
		std::cout << TAB2 << "Set acquisition mode to 'Continuous'\n";

		Arena::SetNodeValue<GenICam::gcstring>(
			pDevice->GetNodeMap(),
			"AcquisitionMode",
			"Continuous");

		// enable stream auto negotiate packet size
		Arena::SetNodeValue<bool>(pDevice->GetTLStreamNodeMap(), "StreamAutoNegotiatePacketSize", true);

		// enable stream packet resend
		Arena::SetNodeValue<bool>(pDevice->GetTLStreamNodeMap(), "StreamPacketResendEnable", true);
	}

	// listener
	else
	{
		std::cout << TAB1 << "Host streaming as 'listener'\n";
	}	

	// start stream
	std::cout << TAB1 << "Start stream\n";

	pDevice->StartStream();

	SaveQueue saveQueue;
	saveQueue.stop = false;
	std::thread saveThread(SaveWorker, &saveQueue);
	SaveWorkerGuard saveGuard = { &saveQueue, &saveThread };

	TerminalGuard terminalGuard = { SetupTerminalForEsc() };

	// define image count to detect if all images are not received
	int imageCount = 0;
	int unreceivedImageCount = 0;
	int savedImageCount = 0;
	bool isMaster = (deviceAccessStatus == "ReadWrite");

	// get images
	if (isMaster)
		std::cout << TAB1 << "Getting images until ESC\n";
	else
		std::cout << TAB1 << "Getting images until 10 saves or ESC\n";

	Arena::IImage* pImage = NULL;

	bool escPressed = false;

	while (true)
	{
		// get image
		imageCount++;
		try
		{
			pImage = pDevice->GetImage(TIMEOUT);
		}
		catch (GenICam::TimeoutException&)
		{
			std::cout << TAB2 << "No image received\n";
			unreceivedImageCount++;
			if (CheckForEsc(terminalGuard.settings))
			{
				escPressed = true;
				break;
			}
			continue;
		}

		// Print identifying information
		//    Using the frame ID and timestamp allows for the comparison of
		//    images between multiple hosts.
		std::cout << TAB2 << "Image retrieved";

		uint64_t frameId = pImage->GetFrameId();
		uint64_t timestampNs = pImage->GetTimestampNs();

		std::cout << " (frame ID " << frameId << "; timestamp (ns): " << timestampNs << ")";

		if (savedImageCount < 10)
		{
			std::ostringstream filename;
			filename << outputDir << "/" << timestampNs << "-" << frameId << ".png";
			SaveJob job;
			// Copy image data so the buffer can be requeued immediately.
			job.pImage = Arena::ImageFactory::Copy(pImage);
			job.filename = filename.str();
			EnqueueSave(&saveQueue, job);
			savedImageCount++;
			std::cout << " - saved: " << filename.str();
		}

		// requeue buffer
		std::cout << " and requeue\n";
		pDevice->RequeueBuffer(pImage);

		if (CheckForEsc(terminalGuard.settings))
			escPressed = true;

		if (escPressed)
			break;

		if (!isMaster && savedImageCount >= 10)
			break;
	}

	if (unreceivedImageCount == imageCount)
	{
		std::cout << "\nNo images were received, this can be caused by firewall or VPN settings\n";
		std::cout << "Please add the application to firewall exception\n\n";
	}
	// stop stream
	std::cout << TAB1 << "Stop stream\n";

	pDevice->StopStream();

	// return node to its initial value
	if (deviceAccessStatus == "ReadWrite")
	{
		Arena::SetNodeValue<GenICam::gcstring>(pDevice->GetNodeMap(), "AcquisitionMode", acquisitionModeInitial);
	}
}

// =-=-=-=-=-=-=-=-=-
// =- PREPARATION -=-
// =- & CLEAN UP =-=-
// =-=-=-=-=-=-=-=-=-

Arena::DeviceInfo SelectDevice(std::vector<Arena::DeviceInfo>& deviceInfos)
{
	if (deviceInfos.size() == 1)
	{
		std::cout << "\n"
				  << TAB1 << "Only one device detected: " << deviceInfos[0].ModelName() << TAB1 << deviceInfos[0].SerialNumber() << TAB1 << deviceInfos[0].IpAddressStr() << ".\n";
		std::cout << TAB1 << "Automatically selecting this device.\n";
		return deviceInfos[0];
	}

	std::cout << "\nSelect device:\n";
	for (size_t i = 0; i < deviceInfos.size(); i++)
	{
		std::cout << TAB1 << i + 1 << ". " << deviceInfos[i].ModelName() << TAB1 << deviceInfos[i].SerialNumber() << TAB1 << deviceInfos[i].IpAddressStr() << "\n";
	}
	size_t selection = 0;

	do
	{
		std::cout << TAB1 << "Make selection (1-" << deviceInfos.size() << "): ";
		std::cin >> selection;

		if (std::cin.fail())
		{
			std::cin.clear();
			while (std::cin.get() != '\n')
				;
			std::cout << TAB1 << "Invalid input. Please enter a number.\n";
		}
		else if (selection <= 0 || selection > deviceInfos.size())
		{
			std::cout << TAB1 << "Invalid device selected. Please select a device in the range (1-" << deviceInfos.size() << ").\n";
		}

	} while (selection <= 0 || selection > deviceInfos.size());

	return deviceInfos[selection - 1];
}

int main(int argc, char** argv)
{
	// flag to track when an exception has been thrown
	bool exceptionThrown = false;

	std::cout << "Cpp_Multicast_Save";

	if (argc < 2)
	{
		std::cout << "\nUsage: " << argv[0] << " <interface>\n";
		std::cout << "Example: " << argv[0] << " eno1\n";
		return 0;
	}

	const char* interfaceName = argv[1];

	try
	{
		// prepare example
		Arena::ISystem* pSystem = Arena::OpenSystem();
		pSystem->UpdateDevices(100);
		std::vector<Arena::DeviceInfo> deviceInfos = pSystem->GetDevices();
		if (deviceInfos.size() == 0)
		{
			std::cout << "\nNo camera connected\nPress enter to complete\n";
			std::getchar();
			return 0;
		}
		Arena::DeviceInfo selectedDeviceInfo = SelectDevice(deviceInfos);
		Arena::IDevice* pDevice = pSystem->CreateDevice(selectedDeviceInfo);

		std::string outputDir = CreateOutputDir();
		std::cout << TAB1 << "Output directory: " << outputDir << "\n";

		std::cout << TAB1 << "Join multicast group " << MULTICAST_GROUP_IP << " on " << interfaceName << "\n";
		MulticastGuard multicastGuard;
		multicastGuard.Join(interfaceName);

		// run example
		std::cout << "Commence example\n\n";
		AcquireImages(pDevice, outputDir);
		std::cout << "\nExample complete\n";

		// clean up example
		pSystem->DestroyDevice(pDevice);
		Arena::CloseSystem(pSystem);
	}
	catch (GenICam::GenericException& ge)
	{
		std::cout << "\nGenICam exception thrown: " << ge.what() << "\n";
		exceptionThrown = true;
	}
	catch (std::exception& ex)
	{
		std::cout << "\nStandard exception thrown: " << ex.what() << "\n";
		exceptionThrown = true;
	}
	catch (...)
	{
		std::cout << "\nUnexpected exception thrown\n";
		exceptionThrown = true;
	}

	std::cout << "Press enter to complete\n";
	std::cin.ignore();
	std::getchar();

	if (exceptionThrown)
		return -1;
	else
		return 0;
}
