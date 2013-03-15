/*
 * FaceTracking.cpp
 *
 *  Created on: 19.07.2012
 *      Author: poschmann
 */

#include "FaceTracking.h"
#include "imageio/VideoImageSource.h"
#include "imageio/KinectImageSource.h"
#include "imageio/DirectoryImageSource.h"
#include "imageio/VideoImageSink.h"
#include "imageprocessing/ImagePyramid.hpp"
#include "imageprocessing/PyramidPatchExtractor.hpp"
#include "imageprocessing/IdentityFeatureTransformer.hpp"
#include "imageprocessing/FilteringFeatureTransformer.hpp"
#include "imageprocessing/GrayscaleFilter.hpp"
#include "imageprocessing/HistogramEqualizationFilter.hpp"
#include "imageprocessing/HistEq64Filter.hpp"
#include "imageprocessing/FeatureExtractor.hpp"
#include "classification/WvmClassifier.hpp"
#include "classification/SvmClassifier.hpp"
#include "classification/ProbabilisticWvmClassifier.hpp"
#include "classification/ProbabilisticSvmClassifier.hpp"
#include "condensation/ResamplingSampler.h"
#include "condensation/GridSampler.h"
#include "condensation/LowVarianceSampling.h"
#include "condensation/SimpleTransitionModel.h"
#include "condensation/WvmSvmModel.h"
#include "condensation/SelfLearningMeasurementModel.h"
#include "condensation/PositionDependentMeasurementModel.h"
#include "condensation/FilteringPositionExtractor.h"
#include "condensation/WeightedMeanPositionExtractor.h"
#include "condensation/Rectangle.h"
#include "condensation/Sample.h"
#include <boost/optional.hpp>
#include <boost/program_options.hpp>
#include <vector>
#include <iostream>
#ifdef WIN32
	#include "wingettimeofday.h"
#else
	#include <sys/time.h>
#endif

using namespace imageprocessing;
using namespace classification;
using std::move;

namespace po = boost::program_options;

const string FaceTracking::videoWindowName = "Image";
const string FaceTracking::controlWindowName = "Controls";

FaceTracking::FaceTracking(unique_ptr<ImageSource> imageSource, unique_ptr<ImageSink> imageSink,
		string svmConfigFile, string negativesFile) :
				svmConfigFile(svmConfigFile),
				negativesFile(negativesFile),
				imageSource(move(imageSource)),
				imageSink(move(imageSink)) {
	initTracking();
	initGui();
}

FaceTracking::~FaceTracking() {}

void FaceTracking::initTracking() {
	// create measurement model
	shared_ptr<ImagePyramid> pyramid = make_shared<ImagePyramid>(20.0 / 480.0, 20.0 / 80.0, 0.85);
	pyramid->addImageFilter(make_shared<GrayscaleFilter>());
	shared_ptr<PatchExtractor> patchExtractor = make_shared<PyramidPatchExtractor>(pyramid, 20, 20);
	shared_ptr<FilteringFeatureTransformer> featureTransformer = make_shared<FilteringFeatureTransformer>(make_shared<IdentityFeatureTransformer>());
	//featureTransformer->add(make_shared<HistogramEqualizationFilter>());
	featureTransformer->add(make_shared<HistEq64Filter>());
	shared_ptr<FeatureExtractor> featureExtractor = make_shared<FeatureExtractor>(patchExtractor, featureTransformer);
	string svmConfigFile1 = "/home/poschmann/projects/ffd/config/fdetection/WRVM/fd_web/fnf-hq64-wvm_big-outnew02-hq64SVM/fd_hq64-fnf_wvm_r0.04_c1_o8x8_n14l20t10_hcthr0.72-0.27,0.36-0.14--With-outnew02-HQ64SVM.mat";
	string svmConfigFile2 = "/home/poschmann/projects/ffd/config/fdetection/WRVM/fd_web/fnf-hq64-wvm_big-outnew02-hq64SVM/fd_hq64-fnf_wvm_r0.04_c1_o8x8_n14l20t10_hcthr0.72-0.27,0.36-0.14--ts107742-hq64_thres_0.005--with-outnew02HQ64SVM.mat";
	shared_ptr<ProbabilisticWvmClassifier> wvm = ProbabilisticWvmClassifier::loadMatlab(svmConfigFile1, svmConfigFile2);
	shared_ptr<ProbabilisticSvmClassifier> svm = ProbabilisticSvmClassifier::loadMatlab(svmConfigFile1, svmConfigFile2);
//	wvm->getWvm()->setLimitReliability(3.2f);
//	svm->getSvm()->setLimitReliability(3.2f);
	measurementModel = make_shared<WvmSvmModel>(featureExtractor, wvm, svm);

	// create tracker
	unsigned int count = 800;
	double randomRate = 0.35;
	transitionModel = make_shared<SimpleTransitionModel>(0.2);
	resamplingSampler = make_shared<ResamplingSampler>(count, randomRate, make_shared<LowVarianceSampling>(),
			transitionModel, 0.1666, 0.8);
	gridSampler = make_shared<GridSampler>(0.1666, 0.8, 1 / 0.85, 0.1);
	tracker = unique_ptr<CondensationTracker>(new CondensationTracker(
			resamplingSampler, measurementModel, make_shared<FilteringPositionExtractor>(make_shared<WeightedMeanPositionExtractor>())));
}

void FaceTracking::initGui() {
	drawSamples = true;

	cvNamedWindow(controlWindowName.c_str(), CV_WINDOW_AUTOSIZE);
	cvMoveWindow(controlWindowName.c_str(), 750, 50);

	cv::createTrackbar("Grid/Resampling", controlWindowName, NULL, 1, samplerChanged, this);
	cv::setTrackbarPos("Grid/Resampling", controlWindowName, tracker->getSampler() == gridSampler ? 0 : 1);

	cv::createTrackbar("Sample Count", controlWindowName, NULL, 2000, sampleCountChanged, this);
	cv::setTrackbarPos("Sample Count", controlWindowName, resamplingSampler->getCount());

	cv::createTrackbar("Random Rate", controlWindowName, NULL, 100, randomRateChanged, this);
	cv::setTrackbarPos("Random Rate", controlWindowName, 100 * resamplingSampler->getRandomRate());

	cv::createTrackbar("Scatter * 100", controlWindowName, NULL, 100, scatterChanged, this);
	cv::setTrackbarPos("Scatter * 100", controlWindowName, 100 * transitionModel->getScatter());

	cv::createTrackbar("Draw samples", controlWindowName, NULL, 1, drawSamplesChanged, this);
	cv::setTrackbarPos("Draw samples", controlWindowName, drawSamples ? 1 : 0);
}

void FaceTracking::samplerChanged(int state, void* userdata) {
	FaceTracking *tracking = (FaceTracking*)userdata;
	if (state == 0)
		tracking->tracker->setSampler(tracking->gridSampler);
	else
		tracking->tracker->setSampler(tracking->resamplingSampler);
}

void FaceTracking::sampleCountChanged(int state, void* userdata) {
	FaceTracking *tracking = (FaceTracking*)userdata;
	tracking->resamplingSampler->setCount(state);
}

void FaceTracking::randomRateChanged(int state, void* userdata) {
	FaceTracking *tracking = (FaceTracking*)userdata;
	tracking->resamplingSampler->setRandomRate(0.01 * state);
}

void FaceTracking::scatterChanged(int state, void* userdata) {
	FaceTracking *tracking = (FaceTracking*)userdata;
	tracking->transitionModel->setScatter(0.01 * state);
}

void FaceTracking::drawSamplesChanged(int state, void* userdata) {
	FaceTracking *tracking = (FaceTracking*)userdata;
	tracking->drawSamples = (state == 1);
}

void FaceTracking::drawDebug(cv::Mat& image) {
	cv::Scalar black(0, 0, 0); // blue, green, red
	cv::Scalar red(0, 0, 255); // blue, green, red
	if (drawSamples) {
		const std::vector<Sample> samples = tracker->getSamples();
		for (auto sample = samples.cbegin(); sample != samples.cend(); ++sample) {
			cv::Scalar color = sample->isObject() ? cv::Scalar(0, 0, sample->getWeight() * 255) : black;
			cv::circle(image, cv::Point(sample->getX(), sample->getY()), 3, color);
		}
	}
	cv::circle(image, cv::Point(10, 10), 5, red, -1);
}

void FaceTracking::run() {
	running = true;
	paused = false;

	bool first = true;
	cv::Mat frame, image;
	cv::Scalar red(0, 0, 255); // blue, green, red
	cv::Scalar green(0, 255, 0); // blue, green, red

	timeval start, detStart, detEnd, frameStart, frameEnd;
	float allIterationTimeSeconds = 0, allDetectionTimeSeconds = 0;

	int frames = 0;
	gettimeofday(&start, 0);
	std::cout.precision(2);

	while (running) {
		frames++;
		gettimeofday(&frameStart, 0);
		frame = imageSource->get();

		if (frame.empty()) {
			std::cerr << "Could not capture frame - press 'q' to quit program" << std::endl;
			stop();
			while ('q' != (char)cv::waitKey(10));
		} else {
			if (first) {
				first = false;
				image.create(frame.rows, frame.cols, frame.type());
			}
			gettimeofday(&detStart, 0);
			boost::optional<Rectangle> face = tracker->process(frame);
			gettimeofday(&detEnd, 0);
			image = frame;
			drawDebug(image);
			if (face) {
				cv::rectangle(image, cv::Point(face->getX(), face->getY()),
						cv::Point(face->getX() + face->getWidth(), face->getY() + face->getHeight()), red);
			}
			imshow(videoWindowName, image);
			if (imageSink.get() != 0)
				imageSink->add(image);
			gettimeofday(&frameEnd, 0);

			int iterationTimeMilliseconds = 1000 * (frameEnd.tv_sec - frameStart.tv_sec) + (frameEnd.tv_usec - frameStart.tv_usec) / 1000;
			int detectionTimeMilliseconds = 1000 * (detEnd.tv_sec - detStart.tv_sec) + (detEnd.tv_usec - detStart.tv_usec) / 1000;
			allIterationTimeSeconds += 0.001 * iterationTimeMilliseconds;
			allDetectionTimeSeconds += 0.001 * detectionTimeMilliseconds;
			float iterationFps = frames / allIterationTimeSeconds;
			float detectionFps = frames / allDetectionTimeSeconds;
			std::cout << "frame: " << frames << "; time: "
					<< iterationTimeMilliseconds << " ms (" << iterationFps << " fps); detection: "
					<< detectionTimeMilliseconds << " ms (" << detectionFps << " fps)" << std::endl;

			int delay = paused ? 0 : 5;
			char c = (char)cv::waitKey(delay);
			if (c == 'p')
				paused = !paused;
			else if (c == 'q')
				stop();
		}
	}
}

void FaceTracking::stop() {
	running = false;
}

int main(int argc, char *argv[]) {
	// TODO svmConfigFile will be ignored by now
	int verboseLevelText;
	int verboseLevelImages;
	int deviceId, kinectId;
	string filename, directory;
	bool useCamera = false, useKinect = false, useFile = false, useDirectory = false;
	string svmConfigFile, negativeRtlPatches;
	string outputFile;
	int outputFps = -1;

	try {
		po::options_description desc("Allowed options");
		desc.add_options()
			("help,h", "Produce help message")
			("verbose-text,v", po::value<int>(&verboseLevelText)->implicit_value(2)->default_value(0,"minimal text output"), "Enable text-verbosity (optionally specify level)")
			("verbose-images,w", po::value<int>(&verboseLevelImages)->implicit_value(2)->default_value(0,"minimal image output"), "Enable image-verbosity (optionally specify level)")
			("filename,f", po::value< string >(&filename), "A filename of a video to run the tracking")
			("directory,i", po::value< string >(&directory), "Use a directory as input")
			("device,d", po::value<int>(&deviceId)->implicit_value(0), "A camera device ID for use with the OpenCV camera driver")
			("kinect,k", po::value<int>(&kinectId)->implicit_value(0), "Windows only: Use a Kinect as camera. Optionally specify a device ID.")
			("config,c", po::value< string >(&svmConfigFile)->default_value("fd_config_fft_fd.mat","fd_config_fft_fd.mat"), "The filename to the config file that contains the SVM and WVM classifiers.")
			("nonfaces,n", po::value< string >(&negativeRtlPatches)->default_value("nonfaces_1000","nonfaces_1000"), "Filename to a file containing the static negative training examples for the real-time learning SVM.")
			("output,o", po::value< string >(&outputFile)->default_value("","none"), "Filename to a video file for storing the image data.")
			("output-fps,r", po::value<int>(&outputFps)->default_value(-1), "The framerate of the output video.")
			;

		po::variables_map vm;
		po::store(po::parse_command_line(argc, argv, desc), vm);
		po::notify(vm);

		if (vm.count("help")) {
			std::cout << "Usage: faceTrackingApp [options]" << std::endl;
			std::cout << desc;
			return 0;
		}
		if (vm.count("filename"))
			useFile = true;
		if (vm.count("directory"))
			useDirectory = true;
		if (vm.count("device"))
			useCamera = true;
		if (vm.count("kinect"))
			useKinect = true;
	}
	catch (std::exception& e) {
		std::cout << e.what() << std::endl;
		return -1;
	}

	int inputsSpecified = 0;
	if (useCamera)
		inputsSpecified++;
	if (useKinect)
		inputsSpecified++;
	if (useFile)
		inputsSpecified++;
	if (useDirectory)
		inputsSpecified++;
	if (inputsSpecified != 1) {
		std::cout << "Usage: Please specify a camera, Kinect, file or directory (and only one of them) to run the program. Use -h for help." << std::endl;
		return -1;
	}

	// TODO new logging stuff
//	Logger->setVerboseLevelText(verboseLevelText);
//	Logger->setVerboseLevelImages(verboseLevelImages);

	unique_ptr<ImageSource> imageSource;
	if (useCamera)
		imageSource.reset(new VideoImageSource(deviceId));
	else if (useKinect)
		imageSource.reset(new KinectImageSource(kinectId));
	else if (useFile)
		imageSource.reset(new VideoImageSource(filename));
	else if (useDirectory)
		imageSource.reset(new DirectoryImageSource(directory));

	unique_ptr<ImageSink> imageSink;
	if (outputFile != "") {
		if (outputFps < 0) {
			std::cout << "Usage: You have to specify the framerate of the output video file by using option -r. Use -h for help." << std::endl;
			return -1;
		}
		imageSink.reset(new VideoImageSink(outputFile, outputFps));
	}

	unique_ptr<FaceTracking> tracker(new FaceTracking(move(imageSource), move(imageSink), svmConfigFile, negativeRtlPatches));
	tracker->run();
	return 0;
}
