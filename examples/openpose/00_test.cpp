#include <opencv2/opencv.hpp>
#include <filesystem>
#include <string>

#define OPENPOSE_FLAGS_DISABLE_POSE
#include <openpose/flags.hpp>
#include <openpose/headers.hpp>

// Defaults point to the bind-mounted folder inside the container
DEFINE_string(image_path, "/opt/openpose/examples/media/COCO_val2014_000000000589.jpg",
              "Input image path.");
DEFINE_string(output_dir, "/opt/openpose/examples/media",
              "Where the rendered image will be saved.");
DEFINE_string(output_name, "",
              "Optional output filename; if empty, uses <input>_pose.png.");
DEFINE_bool(no_display, true, "Disable visual display.");

static std::string deriveOutputPath(const std::string& inputPath,
                                    const std::string& outDir,
                                    const std::string& outName)
{
    namespace fs = std::filesystem;
    fs::path dir(outDir);
    if (!fs::exists(dir)) fs::create_directories(dir);

    if (!outName.empty()) return (dir / outName).string();

    fs::path in(inputPath);
    std::string stem = in.has_stem() ? in.stem().string() : "output";
    return (dir / (stem + "_pose.png")).string();
}

void saveOutput(const std::shared_ptr<std::vector<std::shared_ptr<op::Datum>>>& datumsPtr)
{
    try {
        if (datumsPtr && !datumsPtr->empty()) {
            const cv::Mat cvMat = OP_OP2CVCONSTMAT(datumsPtr->at(0)->cvOutputData);
            if (!cvMat.empty()) {
                const auto outPath = deriveOutputPath(FLAGS_image_path, FLAGS_output_dir, FLAGS_output_name);
                if (!cv::imwrite(outPath, cvMat))
                    op::opLog("Failed to write image to: " + outPath, op::Priority::High);
                else
                    op::opLog("Saved rendered pose image to: " + outPath, op::Priority::High);
            } else {
                op::opLog("Empty cv::Mat as output.", op::Priority::High);
            }
        } else {
            op::opLog("Nullptr or empty datumsPtr found.", op::Priority::High);
        }
    } catch (const std::exception& e) {
        op::error(e.what(), __LINE__, __FUNCTION__, __FILE__);
    }
}

void printKeypoints(const std::shared_ptr<std::vector<std::shared_ptr<op::Datum>>>& datumsPtr)
{
    try {
        if (datumsPtr && !datumsPtr->empty())
            op::opLog("Body keypoints: " + datumsPtr->at(0)->poseKeypoints.toString(), op::Priority::High);
        else
            op::opLog("Nullptr or empty datumsPtr found.", op::Priority::High);
    } catch (const std::exception& e) {
        op::error(e.what(), __LINE__, __FUNCTION__, __FILE__);
    }
}

int tutorialApiCpp()
{
    try {
        op::opLog("Starting OpenPose demo...", op::Priority::High);
        const auto opTimer = op::getTimerInit();

        op::Wrapper opWrapper{op::ThreadManagerMode::Asynchronous};
        if (FLAGS_disable_multi_thread) opWrapper.disableMultiThreading();
        opWrapper.start();

        const cv::Mat cvImageToProcess = cv::imread(FLAGS_image_path);
        if (cvImageToProcess.empty()) {
            op::opLog("Could not read input image: " + FLAGS_image_path, op::Priority::High);
            return -1;
        }

        const op::Matrix imageToProcess = OP_CV2OPCONSTMAT(cvImageToProcess);
        auto datumProcessed = opWrapper.emplaceAndPop(imageToProcess);
        if (datumProcessed) {
            printKeypoints(datumProcessed);
            saveOutput(datumProcessed);
        } else {
            op::opLog("Image could not be processed.", op::Priority::High);
        }

        op::printTime(opTimer, "OpenPose demo successfully finished. Total time: ",
                      " seconds.", op::Priority::High);
        return 0;
    } catch (const std::exception&) {
        return -1;
    }
}

int main(int argc, char* argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    return tutorialApiCpp();
}
