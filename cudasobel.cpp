/*
This file is a cuda implementation of sobel filter, which takes a batch of images of size [width*height*batch_size]
The sobel filter is a modified method provided originally by claudiu, the original site is :
http://www.coldvision.io/2016/03/18/image-gradient-sobel-operator-opencv-3-x-cuda/
*/
#include "opencv2/opencv.hpp"
#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/nonfree/features2d.hpp"



const int sobel_width = 3;
__constant__ int c_sobel_x[sobel_width][sobel_width];   //sobel filter on the x axis
__constant__ int c_sobel_y[sobel_width][sobel_width];   //sobel filter on the y axis

// the kernel matrix is stored as array (1,r*r) instead of (r,r)
inline void setSobelKernels()
{
	const int numElements = sobel_width * sobel_width;
	const int h_sobel_x[sobel_width][sobel_width] = {{-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1}};
	const int h_sobel_y[sobel_width][sobel_width] = {{-1, -2, -1}, {0, 0, 0}, {1, 2, 1}};
	SAFE_CALL( cudaMemcpyToSymbol(c_sobel_x, h_sobel_x, sizeof(int)*numElements), "CUDA Kernel Memcpy Host To Device Failed");
	SAFE_CALL( cudaMemcpyToSymbol(c_sobel_y, h_sobel_y, sizeof(int)*numElements), "CUDA Kernel Memcpy Host To Device Failed");
}

// this method applies both sobel filters for x and yon d_input and saves the combined result into d_output
// d_input and d_output are 1-channel data
__global__ void applySobelFilters(const uchar* const d_input,
		const size_t width, const size_t height,
		const int kernel_width,
		uchar* const d_output
)
{
	extern __shared__ uchar s_input2[];

	//2D Index of current thread
	const int x = blockIdx.x * blockDim.x + threadIdx.x;
	const int y = blockIdx.y * blockDim.y + threadIdx.y;

	//Only valid threads perform memory I/O
	if((x<width) && (y<height))
	{
	    /*modified shared memory*/
		const int crtShareIndex = threadIdx.z * blockDim.x * blockDim.y + threadIdx.y * blockDim.x + threadIdx.x;
		const int crtGlobalIndex = threadIdx.z * width * height + y * width + x;
		s_input2[crtShareIndex] = d_input[crtGlobalIndex];
		__syncthreads();

		const int r = (kernel_width - 1) / 2;
		int sum_x = 0;
		int sum_y = 0;

		for (int i = -r; i <= r; ++i)
		{
			int crtY = threadIdx.y + i; //clamp the neighbor pixel, prevent overflow
			if (crtY < 0)						crtY = 0;
			else if (crtY >= blockDim.y)   		crtY = blockDim.y - 1;

			for (int j = -r; j <= r; ++j)
			{
				int crtX = threadIdx.x + j;
				if (crtX < 0) 					crtX = 0;
				else if (crtX >= blockDim.x)	crtX = blockDim.x - 1;
                /*modified*/
				const float inputPix = (float)(s_input2[threadIdx.z * blockDim.x * blockDim.y + crtY * blockDim.x + crtX]);
				sum_x += inputPix * c_sobel_x[r + j][r + i];
				sum_y += inputPix * c_sobel_y[r + j][r + i];
			}
		}
        /*modified */
		d_output[threadIdx.z * width * height + y * width + x] = (uchar) (abs(sum_x) + abs(sum_y));
	}
}

// this method does all operations required by the sobel filter:
// upload to GPU, convert to grayscale, gaussian filter and sobel operator, download from GPU
/*modified: for now pass in dimensions*/
void sobelFilterCuda(const cv::Mat& input, cv::Mat& output, size_t weight, size_t height, size_t batch_sz)
{
    /*modified: to batch let's now assume input and output are HUGE continuous images*/ 
	const size_t numElemts = weight * height * batch_sz;

	//Allocate device memory
	uchar3 *d_input, *d_inputBlurred; // CV_U8C3
	uchar *d_inputGrayscale, *d_output; // CV_U8C1
	SAFE_CALL(cudaMalloc<uchar3>(&d_input, numElemts * sizeof(uchar3)), "CUDA Malloc Failed");
	SAFE_CALL(cudaMalloc<uchar3>(&d_inputBlurred, numElemts * sizeof(uchar3)), "CUDA Malloc Failed");
	SAFE_CALL(cudaMalloc<uchar>(&d_inputGrayscale, numElemts * sizeof(uchar)), "CUDA Malloc Failed");
	SAFE_CALL(cudaMalloc<uchar>(&d_output, numElemts * sizeof(uchar)), "CUDA Malloc Failed");

	//Copy data from OpenCV input image to device memory
	SAFE_CALL(cudaMemcpy(d_input, input.ptr<uchar3>(), numElemts * sizeof(uchar3), cudaMemcpyHostToDevice), "CUDA Memcpy Host To Device Failed");

	// set default kernel size as 16x16 to efficiently use the shared memory
	/*modified: block to 3D*/
	const dim3 blockDimHist(16, 16, batch_sz);
	const dim3 gridDimHist(ceil((float)input.cols/blockDimHist.x), ceil((float)input.rows/blockDimHist.y), 1);
	size_t blockSharedMemory = 0;


	// 1) blur the input image using the gaussian filter to remove the noise

	// compute the gaussian kernel for the current radius and delta
	const float euclideanDelta = 1.0f;
	const int filterRadius = 3;
	computeGaussianKernelCuda(euclideanDelta, filterRadius);

	// apply the gaussian kernel
	blockSharedMemory = blockDimHist.x * blockDimHist.y * blockDimHist.z * sizeof(float3);
	applyGaussianFilter<<< gridDimHist, blockDimHist, blockSharedMemory>>>(d_input, input.cols, input.rows,
			euclideanDelta, filterRadius, d_inputBlurred);
	SAFE_CALL(cudaDeviceSynchronize(), "Kernel Launch Failed"); //Synchronize to check for any kernel launch errors


	// 2) convert it to grayscale (CV_8UC3 -> CV_8UC1), +2ms
	convertToGrayscale<<< gridDimHist, blockDimHist, blockSharedMemory>>>(d_inputBlurred, input.cols, input.rows, d_inputGrayscale);
	SAFE_CALL(cudaDeviceSynchronize(), "Kernel Launch Failed"); //Synchronize to check for any kernel launch errors


	// 3) compute the gradients on both directions x and y, and combine the result into d_output

	// set the sobel kernels for both x and y axes and copy them to the constant memory for a faster memory access
	setSobelKernels();

	// apply the sobel kernels both for x and y at the same time
	/*modified: to 3D*/
	blockSharedMemory = blockDimHist.x * blockDimHist.y * blockDimHist.z * sizeof(uchar);
	applySobelFilters<<< gridDimHist, blockDimHist, blockSharedMemory>>>(d_inputGrayscale, input.cols, input.rows, sobel_width, d_output);
	SAFE_CALL(cudaDeviceSynchronize(), "Kernel Launch Failed"); //Synchronize to check for any kernel launch errors


	// Copy back data from destination device memory to OpenCV output image
	SAFE_CALL(cudaMemcpy(output.ptr(), d_output, numElemts * sizeof(uchar), cudaMemcpyDeviceToHost), "CUDA Memcpy Host To Device Failed");

	//Free the device memory
	SAFE_CALL(cudaFree(d_input), "CUDA Free Failed");
	SAFE_CALL(cudaFree(d_inputBlurred), "CUDA Free Failed");
	SAFE_CALL(cudaFree(d_inputGrayscale), "CUDA Free Failed");
	SAFE_CALL(cudaFree(d_output), "CUDA Free Failed");
	//SAFE_CALL(cudaDeviceReset(),"CUDA Device Reset Failed");
}
