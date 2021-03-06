#include <math.h>
#include <vulkan/vulkan.h>

typedef struct {
	//WHDCN layout
	uint32_t size[3] = { 1,1,1 }; // WHD -system dimensions 
	uint32_t coordinateFeatures = 1; // C - coordinate, or dimension of features vector. In matrix convolution - size of vector
	uint32_t matrixConvolution = 1; //if equal to 2 perform 2x2, if equal to 3 perform 3x3 matrix-vector convolution. Overrides coordinateFeatures

	uint32_t numberBatches = 1;// N - used to perform multiple batches of initial data
	uint32_t numberKernels = 1;// N - only used in convolution step - specify how many kernels were initialized before. Expands one input to multiple (batched) output
	uint32_t FFTdim = 1; //FFT dimensionality (1, 2 or 3)
	uint32_t radix = 8; //FFT radix (2, 4 or 8)
	bool performZeropadding[3] = { false, false, false }; // perform zeropadding (false - off, true - on)
	bool performTranspose[2] = { false, false }; //will be selected automatically
	bool performConvolution = false; //perform convolution in this application (false - off, true - on)
	bool performR2C = false; //perform R2C/C2R decomposition (false - off, true - on)
	bool inverse = false; //perform inverse FFT (false - forward, true - inverse)
	bool symmetricKernel = false; //specify if kernel in 2x2 or 3x3 matrix convolution is symmetric
	bool isInputFormatted = false; //specify if input buffer is not padded for R2C if out-of-place mode is selected (only if numberBatches==1 and numberKernels==1) - false - padded, true - not padded
	bool isOutputFormatted = false; //specify if output buffer is not padded for R2C if out-of-place mode is selected (only if numberBatches==1 and numberKernels==1) - false - padded, true - not padded
	uint32_t registerBoost = 1; //specify if register file size is bigger than shared memory (on Nvidia 256KB register file can be used instead of 32KB of shared memory, set this constant to 4)
	char shaderPath[256] = "shaders/"; //path to shaders, can be selected automatically in CMake
	uint32_t coalescedMemory = 32;//in bits, for Nvidia compute capability >=6.0 is equal to 32, <6.0 and Intel is equal 128. Gonna work regardles, but if specified by user correctly, the performance will be higher. 
	VkDevice* device;

	VkDeviceSize* bufferSize;
	VkDeviceSize* inputBufferSize;
	VkDeviceSize* outputBufferSize;

	VkBuffer* buffer;
	VkBuffer* inputBuffer;
	VkBuffer* outputBuffer;

	VkDeviceSize* kernelSize;
	VkBuffer* kernel;
} VkFFTConfiguration;

typedef struct {
	uint32_t localSize[3];
	uint32_t fftDim;
	VkBool32 inverse;
	VkBool32 zeropad[2];
	uint32_t inputStride[5];
	uint32_t outputStride[5];
	uint32_t fft_dim_full;
	uint32_t stageStartSize;
	uint32_t fft_dim_x;
	uint32_t numStages;
	uint32_t stageRadix[2] = { 0,0 };
	uint32_t ratio[2];
	VkBool32 ratioDirection[2];
	uint32_t inputOffset;
	uint32_t outputOffset;
	uint32_t passID;
} VkFFTSpecializationConstantsLayout;

typedef struct {
	uint32_t coordinate=0;
	uint32_t batch=0;
} VkFFTPushConstantsLayout;

typedef struct {
	uint32_t localSize[3];
	uint32_t inputStride[5];
	uint32_t ratio;
	VkBool32 ratioDirection;
} VkFFTTransposeSpecializationConstantsLayout;
typedef struct {
	uint32_t axisBlock[4];
	uint32_t groupedBatch = 16;
	VkFFTSpecializationConstantsLayout specializationConstants;
	VkFFTPushConstantsLayout pushConstants;
	VkDescriptorPool descriptorPool;
	VkDescriptorSetLayout descriptorSetLayout;
	VkDescriptorSet descriptorSet;
	VkPipelineLayout pipelineLayout;
	VkPipeline pipeline;
} VkFFTAxis;
typedef struct {
	uint32_t transposeBlock[3];
	VkFFTTransposeSpecializationConstantsLayout specializationConstants;
	VkFFTPushConstantsLayout pushConstants;
	VkDescriptorPool descriptorPool;
	VkDescriptorSetLayout descriptorSetLayout;
	VkDescriptorSet descriptorSet;
	VkPipelineLayout pipelineLayout;
	VkPipeline pipeline;
} VkFFTTranspose;
typedef struct {
	uint32_t numAxisUploads[3];
	uint32_t numSupportAxisUploads[2];
	VkFFTAxis axes[3][5];
	VkFFTAxis supportAxes[2][5];//Nx/2+1 for r2c/c2r
	VkFFTTranspose transpose[2];

} VkFFTPlan;
typedef struct VkFFTApplication {
	VkFFTConfiguration configuration = {};
	VkFFTPlan localFFTPlan = {};
	VkFFTPlan localFFTPlan_inverse_convolution = {}; //additional inverse plan for convolution.
	uint32_t* VkFFTReadShader(uint32_t& length, const char* filename) {

		FILE* fp = fopen(filename, "rb");
		if (fp == NULL) {
			printf("Could not find or open file: %s\n", filename);
		}

		// get file size.
		fseek(fp, 0, SEEK_END);
		long filesize = ftell(fp);
		fseek(fp, 0, SEEK_SET);

		long filesizepadded = long(ceil(filesize / 4.0)) * 4;

		char* str = (char*)malloc(sizeof(char)*filesizepadded);
		fread(str, filesize, sizeof(char), fp);
		fclose(fp);

		for (long i = filesize; i < filesizepadded; i++) {
			str[i] = 0;
		}

		length = filesizepadded;
		return (uint32_t*)str;
	}
	void VkFFTInitShader(uint32_t shader_id, VkShaderModule* shaderModule) {

		char filename[256];
		switch (shader_id) {
		case 0:
			//printf("vkFFT_single_c2c\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_c2c.spv");
			break;
		case 1:
			//printf("vkFFT_single_c2r\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_c2r.spv");
			break;
		case 2:
			//printf("vkFFT_single_c2c_strided\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_c2c_strided.spv");
			break;
		case 3:
			//printf("vkFFT_single_r2c\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_r2c.spv");
			break;
		case 4:
			//printf("vkFFT_single_r2c_zp\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_r2c_zp.spv");
			break;
		case 5:
			//printf("vkFFT_single_c2c_afterR2C\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_c2c_afterR2C.spv");
			break;
		case 6:
			//printf("vkFFT_single_c2c_beforeC2R\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_c2c_beforeC2R.spv");
			break;
		case 7:
			//printf("vkFFT_grouped_c2c\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_grouped_c2c.spv");
			break;
		case 8:
			//printf("vkFFT_grouped_convolution_1x1\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_grouped_convolution_1x1.spv");
			break;
		case 9:
			//printf("vkFFT_single_convolution_1x1\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_convolution_1x1.spv");
			break;
		case 10:
			//printf("vkFFT_single_strided_convolution_1x1\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_strided_convolution_1x1.spv");
			break;
		case 11:
			//printf("vkFFT_grouped_convolution_symmetric_2x2\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_grouped_convolution_symmetric_2x2.spv");
			break;
		case 12:
			//printf("vkFFT_single_convolution_symmetric_2x2\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_convolution_symmetric_2x2.spv");
			break;
		case 13:
			//printf("vkFFT_single_strided_convolution_symmetric_2x2\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_strided_convolution_symmetric_2x2.spv");
			break;
		case 14:
			//printf("vkFFT_grouped_convolution_nonsymmetric_2x2\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_grouped_convolution_nonsymmetric_2x2.spv");
			break;
		case 15:
			//printf("vkFFT_single_convolution_nonsymmetric_2x2\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_convolution_nonsymmetric_2x2.spv");
			break;
		case 16:
			//printf("vkFFT_single_strided_convolution_nonsymmetric_2x2\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_strided_convolution_nonsymmetric_2x2.spv");
			break;
		case 17:
			//printf("vkFFT_grouped_convolution_symmetric_3x3\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_grouped_convolution_symmetric_3x3.spv");
			break;
		case 18:
			//printf("vkFFT_single_convolution_symmetric_3x3\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_convolution_symmetric_3x3.spv");
			break;
		case 19:
			//printf("vkFFT_single_strided_convolution_symmetric_3x3\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_strided_convolution_symmetric_3x3.spv");
			break;
		case 20:
			//printf("vkFFT_grouped_convolution_nonsymmetric_3x3\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_grouped_convolution_nonsymmetric_3x3.spv");
			break;
		case 21:
			//printf("vkFFT_single_convolution_nonsymmetric_3x3\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_convolution_nonsymmetric_3x3.spv");
			break;
		case 22:
			//printf("vkFFT_single_strided_convolution_nonsymmetric_3x3\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_single_strided_convolution_nonsymmetric_3x3.spv");
			break;
		case 23:
			//printf("vkFFT_single_c2r_8192\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "8192/vkFFT_single_c2r_8192.spv");
			break;
		case 24:
			//printf("vkFFT_single_r2c_8192\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "8192/vkFFT_single_r2c_8192.spv");
			break;
		case 25:
			//printf("vkFFT_single_c2c_8192\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "8192/vkFFT_single_c2c_8192.spv");
			break;
		case 26:
			//printf("vkFFT_grouped_strided_convolution_1x1\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_grouped_strided_convolution_1x1.spv");
			break;
		case 27:
			//printf("vkFFT_grouped_strided_convolution_symmetric_2x2\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_grouped_strided_convolution_symmetric_2x2.spv");
			break;
		case 28:
			//printf("vkFFT_grouped_strided_convolution_nonsymmetric_2x2\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_grouped_strided_convolution_nonsymmetric_2x2.spv");
			break;
		case 29:
			//printf("vkFFT_grouped_strided_convolution_symmetric_3x3\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_grouped_strided_convolution_symmetric_3x3.spv");
			break;
		case 30:
			//printf("vkFFT_grouped_strided_convolution_nonsymmetric_3x3\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_grouped_strided_convolution_nonsymmetric_3x3.spv");
			break;
		case 33:
			//printf("vkFFT_single_c2r_16384\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "16384/vkFFT_single_c2r_16384.spv");
			break;
		case 34:
			//printf("vkFFT_single_r2c_16384\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "16384/vkFFT_single_r2c_16384.spv");
			break;
		case 35:
			//printf("vkFFT_single_c2c_16384\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "16384/vkFFT_single_c2c_16384.spv");
			break;
		case 36:
			//printf("vkFFT_single_c2r_for_transposition_16384\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "16384/vkFFT_single_c2r_for_transposition_16384.spv");
			break;
		case 37:
			//printf("vkFFT_single_r2c_for_transposition_16384\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "16384/vkFFT_single_r2c_for_transposition_16384.spv");
			break;
		case 38:
			//printf("vkFFT_single_c2c_for_transposition_16384\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "16384/vkFFT_single_c2c_for_transposition_16384.spv");
			break;
		case 39:
			//printf("vkFFT_single_c2c_afterR2C_for_transposition_16384\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "16384/vkFFT_single_c2c_afterR2C_for_transposition_16384.spv");
			break;
		case 40:
			//printf("vkFFT_single_c2c_beforeC2R_for_transposition_16384\n");
			sprintf(filename, "%s%s", configuration.shaderPath, "16384/vkFFT_single_c2c_beforeC2R_for_transposition_16384.spv");
			break;
		}


		uint32_t filelength;
		uint32_t* code = VkFFTReadShader(filelength, filename);
		VkShaderModuleCreateInfo createInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		createInfo.pCode = code;
		createInfo.codeSize = filelength;
		vkCreateShaderModule(configuration.device[0], &createInfo, NULL, shaderModule);
		free(code);

	}
	void VkFFTPlanAxis(VkFFTPlan* FFTPlan, uint32_t axis_id, uint32_t axis_upload_id, bool inverse) {
		//get radix stages
		VkFFTAxis* axis = &FFTPlan->axes[axis_id][axis_upload_id];
		
		if (axis_id == 0) {
			//configure radix stages
			uint32_t logSize = log2(configuration.size[axis_id]);
			uint32_t numPasses[8][8];//4096-8k(256KB)-16k(256KB)-32k-64k - find correct strided FFT configuration - x axis | 256-512-1024-2048(256KB)-4096(256KB)-8k(future?)-16k(future?) - find correct strided FFT configuration
			for (uint32_t i = 0; i < 8; i++) {
				for (uint32_t j = 0; j < 8; j++) {
					numPasses[i][j] = 0;
				}
			}
			uint32_t temp = configuration.size[axis_id];
			uint32_t startStage = 4096;
			uint32_t continueStage = 256;
			uint32_t maxPassId[2] = { 0,0 };
			uint32_t minPassId[2] = { 0,0 };
			maxPassId[0] += log2(configuration.registerBoost);
			uint32_t maxSingleSize = 8 * 4096 / configuration.coalescedMemory;
			maxPassId[1] = log2(maxSingleSize / 256);
			minPassId[1] = (maxSingleSize >= 512) ? 1 : 0;
			//maxPassId[1] += log2(configuration.registerBoost);//in development
			for (uint32_t i = 0; i < 8; i++) {
				for (uint32_t j = 0; j < 8; j++) {
					temp /= startStage;
					numPasses[i][j]++;
					while (temp > 1)
					{
						temp /= continueStage;
						numPasses[i][j]++;
					}
					continueStage *= 2;
					temp = configuration.size[axis_id];
				}
				continueStage = 256;
				startStage *= 2;
			}
			uint32_t passId[2] = { minPassId[0], minPassId[1] };
			for (uint32_t i = minPassId[0]; i < maxPassId[0]+1; i++) {
				for (uint32_t j = minPassId[1]; j < maxPassId[1]+1; j++) {
					if (numPasses[i][j] < numPasses[passId[0]][passId[1]]) {
						passId[0] = i;
						passId[1] = j;
					}
				}
			}
			FFTPlan->numAxisUploads[axis_id] = numPasses[passId[0]][passId[1]];
			if (axis_upload_id >= numPasses[passId[0]][passId[1]])
				return;
			if (axis_upload_id == 0) {
				//first pass is non-strided, special case
				switch (configuration.radix) {
				case 8: {
					uint32_t logSize0Pass = (12 + passId[0] < logSize) ? 12 + passId[0] : logSize; //4096 + shift
					if ((axis_upload_id + 1 == numPasses[passId[0]][passId[1]] - 1) && (logSize - logSize0Pass < 3))
						logSize0Pass -= (3 - (logSize - logSize0Pass));
					uint32_t stage8 = logSize0Pass / 3;
					uint32_t stage4 = 0;
					uint32_t stage2 = 0;
					if (logSize0Pass % 3 == 2)
						stage4 = 1;
					if (logSize0Pass % 3 == 1)
						stage2 = 1;
					uint32_t totNumStages = stage8 + stage4 + stage2;

					axis->specializationConstants.numStages = stage8;
					axis->specializationConstants.fftDim = pow(8, stage8);
					axis->specializationConstants.stageRadix[0] = 8;
					axis->specializationConstants.stageRadix[1] = 8;

					if (stage4 == 1) {
						axis->specializationConstants.numStages++;
						axis->specializationConstants.stageRadix[1] = 4;
						axis->specializationConstants.fftDim *= 4;
					}
					if (stage2 == 1) {
						axis->specializationConstants.numStages++;
						axis->specializationConstants.stageRadix[1] = 2;
						axis->specializationConstants.fftDim *= 2;
					}
					axis->specializationConstants.stageStartSize = 1;
					if (configuration.performR2C)
						axis->specializationConstants.fft_dim_x = configuration.size[0] / 2;
					else
						axis->specializationConstants.fft_dim_x = configuration.size[0];

					break;
				}
				case 4: {
					uint32_t stage4 = logSize / 2;
					uint32_t stage2 = 0;
					if (logSize % 2 == 1)
						stage2 = 1;
					axis->specializationConstants.numStages = stage4 + stage2;


					axis->specializationConstants.stageRadix[0] = 4;
					axis->specializationConstants.stageRadix[1] = 4;
					if (logSize % 2 == 1)
						axis->specializationConstants.stageRadix[1] = 2;
					break;
				}
				case 2: {
					uint32_t stage2 = logSize;

					axis->specializationConstants.numStages = stage2;


					axis->specializationConstants.stageRadix[0] = 2;
					axis->specializationConstants.stageRadix[1] = 2;
					break;
				}
				}
			}
			else {
				//passes after first are done similar to strided passes in y and z
				uint32_t logSizeLaterPass = (logSize - 12 - passId[0]<3) ? 3 : logSize - 12 - passId[0]; //4096 + shift
				switch (configuration.radix) {
				case 8: {
					uint32_t stage8 = logSizeLaterPass / 3;
					uint32_t stage4 = 0;
					uint32_t stage2 = 0;
					if (logSizeLaterPass % 3 == 2)
						stage4 = 1;
					if (logSizeLaterPass % 3 == 1)
						stage2 = 1;
					uint32_t totNumStages = stage8 + stage4 + stage2;
					uint32_t locNumStages = 0;
					if (passId[1] == minPassId[1]) {
						locNumStages = stage8 / (numPasses[passId[0]][passId[1]] - 1);
						if (axis_upload_id < stage8 % (numPasses[passId[0]][passId[1]] - 1))
							locNumStages++;
						axis->specializationConstants.numStages = locNumStages;
						axis->specializationConstants.fftDim = pow(8, locNumStages);
						axis->specializationConstants.stageRadix[0] = 8;
						axis->specializationConstants.stageRadix[1] = 8;

						if (axis_upload_id == (numPasses[passId[0]][passId[1]] - 1)) {
							if (stage4 == 1) {
								axis->specializationConstants.numStages++;
								axis->specializationConstants.stageRadix[1] = 4;
								axis->specializationConstants.fftDim *= 4;
							}
							if (stage2 == 1) {
								axis->specializationConstants.numStages++;
								axis->specializationConstants.stageRadix[1] = 2;
								axis->specializationConstants.fftDim *= 2;
							}
						}
						axis->specializationConstants.stageStartSize = FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.stageStartSize * FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.fftDim;
						if (configuration.performR2C)
							axis->specializationConstants.fft_dim_x = configuration.size[0] / 2;
						else
							axis->specializationConstants.fft_dim_x = configuration.size[0];
					}
					else {
						if (axis_upload_id < numPasses[passId[0]][passId[1]] - 1) {
							uint32_t locLogSize = 8 + passId[1];
							if ((axis_upload_id + 1 == numPasses[passId[0]][passId[1]] - 1) && (logSizeLaterPass - (8 + passId[1]) * (numPasses[passId[0]][passId[1]] - 2) < 3))
								locLogSize -= (3 - (logSizeLaterPass - (8 + passId[1]) * (numPasses[passId[0]][passId[1]] - 2)));
							uint32_t locStage8 = locLogSize / 3;
							uint32_t locStage4 = 0;
							uint32_t locStage2 = 0;
							if (locLogSize % 3 == 2)
								locStage4 = 1;
							if (locLogSize % 3 == 1)
								locStage2 = 1;
							axis->specializationConstants.numStages = locStage8 + locStage4 + locStage2;
							axis->specializationConstants.fftDim = pow(2, locLogSize);
							axis->specializationConstants.stageRadix[0] = 8;
							axis->specializationConstants.stageRadix[1] = 8;

							if (locStage4 == 1) {
								axis->specializationConstants.stageRadix[1] = 4;
							}
							if (locStage2 == 1) {
								axis->specializationConstants.stageRadix[1] = 2;
							}
							axis->specializationConstants.stageStartSize = FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.stageStartSize * FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.fftDim;
							if (configuration.performR2C)
								axis->specializationConstants.fft_dim_x = configuration.size[0] / 2;
							else
								axis->specializationConstants.fft_dim_x = configuration.size[0];
						}
						else {
							uint32_t locLogSize = (logSizeLaterPass - (8 + passId[1]) * (numPasses[passId[0]][passId[1]] - 2) < 3) ? 3 : logSizeLaterPass - (8 + passId[1]) * (numPasses[passId[0]][passId[1]] - 2);
							uint32_t locStage8 = locLogSize / 3;
							uint32_t locStage4 = 0;
							uint32_t locStage2 = 0;
							if (locLogSize % 3 == 2)
								locStage4 = 1;
							if (locLogSize % 3 == 1)
								locStage2 = 1;
							axis->specializationConstants.numStages = locStage8 + locStage4 + locStage2;
							axis->specializationConstants.fftDim = pow(2, locLogSize);
							axis->specializationConstants.stageRadix[0] = 8;
							axis->specializationConstants.stageRadix[1] = 8;

							if (locStage4 == 1) {
								axis->specializationConstants.stageRadix[1] = 4;
							}
							if (locStage2 == 1) {
								axis->specializationConstants.stageRadix[1] = 2;
							}
							axis->specializationConstants.stageStartSize = FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.stageStartSize * FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.fftDim;
							if (configuration.performR2C)
								axis->specializationConstants.fft_dim_x = configuration.size[0] / 2;
							else
								axis->specializationConstants.fft_dim_x = configuration.size[0];
						}
					}


					break;
				}
				case 4: {
					uint32_t stage4 = logSize / 2;
					uint32_t stage2 = 0;
					if (logSize % 2 == 1)
						stage2 = 1;
					axis->specializationConstants.numStages = stage4 + stage2;


					axis->specializationConstants.stageRadix[0] = 4;
					axis->specializationConstants.stageRadix[1] = 4;
					if (logSize % 2 == 1)
						axis->specializationConstants.stageRadix[1] = 2;
					break;
				}
				case 2: {
					uint32_t stage2 = logSize;

					axis->specializationConstants.numStages = stage2;


					axis->specializationConstants.stageRadix[0] = 2;
					axis->specializationConstants.stageRadix[1] = 2;
					break;
				}
				}
			}
		}else{
			//configure radix stages
			uint32_t logSize = log2(configuration.size[axis_id]);
			uint32_t numPasses[8] = { 0,0,0,0,0,0,0,0 };//256-512-1024-2048(256KB)-4096(256KB)-8k(future?)-16k(future?) - find correct strided FFT configuration
			uint32_t temp = configuration.size[axis_id];
			uint32_t startStage = 256;
			uint32_t maxSingleSize = 8 * 4096 / configuration.coalescedMemory;
			uint32_t maxPassId = log2(maxSingleSize / 256);
			uint32_t minPassId = (maxSingleSize >= 512) ? 1 : 0;
			//maxPassId += log2(configuration.registerBoost);//in development
			for (uint32_t i = 0; i < 8; i++) {
				while (temp > 1)
				{
					temp /= startStage;
					numPasses[i]++;
				}
				temp = configuration.size[axis_id];
				startStage *= 2;
			}
			uint32_t passId = minPassId;
			for (uint32_t i = minPassId; i < maxPassId+1; i++) {
				if (numPasses[i] < numPasses[passId]) {
					passId = i;
				}
			}
			FFTPlan->numAxisUploads[axis_id] = numPasses[passId];
			if (axis_upload_id >= numPasses[passId])
				return;
			switch (configuration.radix) {
			case 8: {
				uint32_t stage8 = logSize / 3;
				uint32_t stage4 = 0;
				uint32_t stage2 = 0;
				if (logSize % 3 == 2)
					stage4 = 1;
				if (logSize % 3 == 1)
					stage2 = 1;
				uint32_t totNumStages = stage8 + stage4 + stage2;
				uint32_t locNumStages = 0;
				if (passId == minPassId) {
					locNumStages = stage8 / numPasses[passId];
					if (axis_upload_id < stage8 % numPasses[passId])
						locNumStages++;
					axis->specializationConstants.numStages = locNumStages;
					axis->specializationConstants.fftDim = pow(8, locNumStages);
					axis->specializationConstants.stageRadix[0] = 8;
					axis->specializationConstants.stageRadix[1] = 8;

					if (axis_upload_id == numPasses[passId] - 1) {
						if (stage4 == 1) {
							axis->specializationConstants.numStages++;
							axis->specializationConstants.stageRadix[1] = 4;
							axis->specializationConstants.fftDim *= 4;
						}
						if (stage2 == 1) {
							axis->specializationConstants.numStages++;
							axis->specializationConstants.stageRadix[1] = 2;
							axis->specializationConstants.fftDim *= 2;
						}
					}
					axis->specializationConstants.stageStartSize = (axis_upload_id == 0) ? 1 : FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.stageStartSize * FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.fftDim;
					if (configuration.performR2C)
						axis->specializationConstants.fft_dim_x = configuration.size[0] / 2;
					else
						axis->specializationConstants.fft_dim_x = configuration.size[0];
				}
				else {
					if (axis_upload_id < numPasses[passId] - 1) {

						uint32_t locLogSize = 8 + passId;
						if ((axis_upload_id + 1 == numPasses[passId] - 1) && (logSize - (8 + passId) * (numPasses[passId] - 1) < 3))
							locLogSize -= (3 - (logSize - (8 + passId) * (numPasses[passId] - 1)));
						uint32_t locStage8 = locLogSize / 3;
						uint32_t locStage4 = 0;
						uint32_t locStage2 = 0;
						if (locLogSize % 3 == 2)
							locStage4 = 1;
						if (locLogSize % 3 == 1)
							locStage2 = 1;
						axis->specializationConstants.numStages = locStage8 + locStage4 + locStage2;
						axis->specializationConstants.fftDim = pow(2, locLogSize);
						axis->specializationConstants.stageRadix[0] = 8;
						axis->specializationConstants.stageRadix[1] = 8;

						if (locStage4 == 1) {
							axis->specializationConstants.stageRadix[1] = 4;
						}
						if (locStage2 == 1) {
							axis->specializationConstants.stageRadix[1] = 2;
						}
						axis->specializationConstants.stageStartSize = (axis_upload_id == 0) ? 1 : FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.stageStartSize * FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.fftDim;
						if (configuration.performR2C)
							axis->specializationConstants.fft_dim_x = configuration.size[0] / 2;
						else
							axis->specializationConstants.fft_dim_x = configuration.size[0];
					}
					else {
						uint32_t locLogSize = (logSize - (8 + passId)*(numPasses[passId] - 1)<3) ? 3 : logSize - (8 + passId) * (numPasses[passId] - 1);
						uint32_t locStage8 = locLogSize / 3;
						uint32_t locStage4 = 0;
						uint32_t locStage2 = 0;
						if (locLogSize % 3 == 2)
							locStage4 = 1;
						if (locLogSize % 3 == 1)
							locStage2 = 1;
						axis->specializationConstants.numStages = locStage8 + locStage4 + locStage2;
						axis->specializationConstants.fftDim = pow(2, locLogSize);
						axis->specializationConstants.stageRadix[0] = 8;
						axis->specializationConstants.stageRadix[1] = 8;

						if (locStage4 == 1) {
							axis->specializationConstants.stageRadix[1] = 4;
						}
						if (locStage2 == 1) {
							axis->specializationConstants.stageRadix[1] = 2;
						}
						axis->specializationConstants.stageStartSize = (axis_upload_id == 0) ? 1 : FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.stageStartSize * FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.fftDim;
						if (configuration.performR2C)
							axis->specializationConstants.fft_dim_x = configuration.size[0] / 2;
						else
							axis->specializationConstants.fft_dim_x = configuration.size[0];
					}
				}


				break;
			}
			case 4: {
				uint32_t stage4 = logSize / 2;
				uint32_t stage2 = 0;
				if (logSize % 2 == 1)
					stage2 = 1;
				axis->specializationConstants.numStages = stage4 + stage2;


				axis->specializationConstants.stageRadix[0] = 4;
				axis->specializationConstants.stageRadix[1] = 4;
				if (logSize % 2 == 1)
					axis->specializationConstants.stageRadix[1] = 2;
				break;
			}
			case 2: {
				uint32_t stage2 = logSize;

				axis->specializationConstants.numStages = stage2;


				axis->specializationConstants.stageRadix[0] = 2;
				axis->specializationConstants.stageRadix[1] = 2;
				break;
			}
			}
		}

		//axis->groupedBatch = (4096 / axis->specializationConstants.fftDim >= configuration.coalescedMemory / 8) ? 4096 / axis->specializationConstants.fftDim : configuration.coalescedMemory / 8;
		axis->specializationConstants.passID = FFTPlan->numAxisUploads[axis_id] - 1 - axis_upload_id;
		axis->specializationConstants.fft_dim_full = configuration.size[axis_id];
		axis->groupedBatch = (4096 / axis->specializationConstants.fftDim >= configuration.coalescedMemory / 8) ? 4096 / axis->specializationConstants.fftDim : configuration.coalescedMemory / 8;
		//axis->groupedBatch = ((axis_upload_id > 0) && (axis->groupedBatch > axis->specializationConstants.stageStartSize)) ? axis->specializationConstants.stageStartSize : axis->groupedBatch;
		/*if (4096 / configuration.size[1] > configuration.coalescedMemory / 16) {
			configuration.performTranspose[0] = false;
			FFTPlan->groupedBatch = 4096 / configuration.size[1];
		}
		else {
			configuration.performTranspose[0] = true;
		}

		if (4096 / configuration.size[2] > configuration.coalescedMemory / 16) {
			configuration.performTranspose[1] = false;
			FFTPlan->axes[2].groupedBatch = 4096 / configuration.size[2];
		}
		else {
			configuration.performTranspose[1] = true;
		}*/
		//configure strides
		if (configuration.performR2C)
		{
			//perform r2c
			axis->specializationConstants.inputStride[0] = 1;
			axis->specializationConstants.inputStride[3] = (configuration.size[0] / 2 + 1) * configuration.size[1] * configuration.size[2];
			if (axis_id == 0) {
				axis->specializationConstants.inputStride[1] = configuration.size[0];
				axis->specializationConstants.inputStride[2] = (configuration.size[0] / 2 + 1) * configuration.size[1];
			}
			if (axis_id == 1)
			{
				if (configuration.performTranspose[0]) {
					//transpose 0-1
					axis->specializationConstants.inputStride[1] = configuration.size[1];
					axis->specializationConstants.inputStride[2] = (configuration.size[0] / 2 + 1) * configuration.size[1];
				}
				else {
					//don't transpose
					axis->specializationConstants.inputStride[1] = configuration.size[0] / 2;
					axis->specializationConstants.inputStride[2] = (configuration.size[0] / 2 + 1) * configuration.size[1];
				}
			}
			if (axis_id == 2)
			{

				if (configuration.performTranspose[1]) {
					//transpose 0-1, transpose 1-2
					axis->specializationConstants.inputStride[1] = (configuration.size[0] / 2 + 1) * configuration.size[2];
					axis->specializationConstants.inputStride[2] = configuration.size[2];
				}
				else {

					if (configuration.performTranspose[0]) {
						//transpose 0-1, don't transpose 1-2
						axis->specializationConstants.inputStride[1] = (configuration.size[0] / 2 + 1) * configuration.size[1];
						axis->specializationConstants.inputStride[2] = configuration.size[1];
					}
					else {
						//don't transpose
						axis->specializationConstants.inputStride[1] = (configuration.size[0] / 2 + 1) * configuration.size[1];
						axis->specializationConstants.inputStride[2] = configuration.size[0] / 2;
					}
				}
			}

			axis->specializationConstants.outputStride[0] = axis->specializationConstants.inputStride[0];
			axis->specializationConstants.outputStride[1] = axis->specializationConstants.inputStride[1];
			axis->specializationConstants.outputStride[2] = axis->specializationConstants.inputStride[2];
			axis->specializationConstants.outputStride[3] = axis->specializationConstants.inputStride[3];
			if (axis_id == 0) {
				if ((axis_upload_id==0)&&(configuration.isInputFormatted) && (!inverse)) {
					if (configuration.performZeropadding[0])
						axis->specializationConstants.inputStride[1] = configuration.size[0] / 2;

					if (configuration.performZeropadding[1])
						axis->specializationConstants.inputStride[2] = axis->specializationConstants.inputStride[1] * configuration.size[1] / 4;
					else
						axis->specializationConstants.inputStride[2] = axis->specializationConstants.inputStride[1] * configuration.size[1] / 2;

					if (configuration.performZeropadding[2])
						axis->specializationConstants.inputStride[3] = axis->specializationConstants.inputStride[2] * configuration.size[2] / 2;
					else
						axis->specializationConstants.inputStride[3] = axis->specializationConstants.inputStride[2] * configuration.size[2];
				}
				if ((axis_upload_id == FFTPlan->numAxisUploads[axis_id]-1) && (configuration.isOutputFormatted) && ((inverse) || ((configuration.performConvolution) && (configuration.FFTdim == 1)))) {
					if (configuration.performZeropadding[0])
						axis->specializationConstants.outputStride[1] = configuration.size[0] / 2;

					if (configuration.performZeropadding[1])
						axis->specializationConstants.outputStride[2] = axis->specializationConstants.outputStride[1] * configuration.size[1] / 4;
					else
						axis->specializationConstants.outputStride[2] = axis->specializationConstants.outputStride[1] * configuration.size[1] / 2;

					if (configuration.performZeropadding[2])
						axis->specializationConstants.outputStride[3] = axis->specializationConstants.outputStride[2] * configuration.size[2] / 2;
					else
						axis->specializationConstants.outputStride[3] = axis->specializationConstants.outputStride[2] * configuration.size[2];
				}
			}
		}
		else {
			//don't perform r2c
			axis->specializationConstants.inputStride[0] = 1;
			axis->specializationConstants.inputStride[3] = configuration.size[0] * configuration.size[1] * configuration.size[2];
			if (axis_id == 0) {
				axis->specializationConstants.inputStride[1] = configuration.size[0];
				axis->specializationConstants.inputStride[2] = configuration.size[0] * configuration.size[1];
			}
			if (axis_id == 1)
			{
				if (configuration.performTranspose[0]) {
					//transpose 0-1, no transpose 1-2
					axis->specializationConstants.inputStride[1] = configuration.size[1];
					axis->specializationConstants.inputStride[2] = configuration.size[0] * configuration.size[1];
				}
				else {
					//no transpose
					axis->specializationConstants.inputStride[1] = configuration.size[0];
					axis->specializationConstants.inputStride[2] = configuration.size[0] * configuration.size[1];
				}
			}
			if (axis_id == 2)
			{

				if (configuration.performTranspose[1]) {
					//transpose 0-1, transpose 1-2
					axis->specializationConstants.inputStride[1] = configuration.size[0] * configuration.size[2];
					axis->specializationConstants.inputStride[2] = configuration.size[2];
				}
				else {

					if (configuration.performTranspose[0]) {
						//transpose 0-1, no transpose 1-2
						axis->specializationConstants.inputStride[1] = configuration.size[0] * configuration.size[1];
						axis->specializationConstants.inputStride[2] = configuration.size[1];
					}
					else {
						//no transpose
						axis->specializationConstants.inputStride[1] = configuration.size[0] * configuration.size[1];
						axis->specializationConstants.inputStride[2] = configuration.size[0];
					}
				}
			}

			axis->specializationConstants.outputStride[0] = axis->specializationConstants.inputStride[0];
			axis->specializationConstants.outputStride[1] = axis->specializationConstants.inputStride[1];
			axis->specializationConstants.outputStride[2] = axis->specializationConstants.inputStride[2];
			axis->specializationConstants.outputStride[3] = axis->specializationConstants.inputStride[3];
			if (axis_id == 0) {
				if ((axis_upload_id == 0) && (configuration.isInputFormatted) && (!inverse)) {
					if (configuration.performZeropadding[0])
						axis->specializationConstants.inputStride[1] = configuration.size[0] / 2;

					if (configuration.performZeropadding[1])
						axis->specializationConstants.inputStride[2] = axis->specializationConstants.inputStride[1] * configuration.size[1] / 2;
					else
						axis->specializationConstants.inputStride[2] = axis->specializationConstants.inputStride[1] * configuration.size[1];

					if (configuration.performZeropadding[2])
						axis->specializationConstants.inputStride[3] = axis->specializationConstants.inputStride[2] * configuration.size[2] / 2;
					else
						axis->specializationConstants.inputStride[3] = axis->specializationConstants.inputStride[2] * configuration.size[2];
				}
				if ((axis_upload_id == FFTPlan->numAxisUploads[axis_id]-1) && (configuration.isOutputFormatted) && ((inverse) || ((configuration.performConvolution) && (configuration.FFTdim == 1)))) {
					if (configuration.performZeropadding[0])
						axis->specializationConstants.outputStride[1] = configuration.size[0] / 2;

					if (configuration.performZeropadding[1])
						axis->specializationConstants.outputStride[2] = axis->specializationConstants.outputStride[1] * configuration.size[1] / 2;
					else
						axis->specializationConstants.outputStride[2] = axis->specializationConstants.outputStride[1] * configuration.size[1];

					if (configuration.performZeropadding[2])
						axis->specializationConstants.outputStride[3] = axis->specializationConstants.outputStride[2] * configuration.size[2] / 2;
					else
						axis->specializationConstants.outputStride[3] = axis->specializationConstants.outputStride[2] * configuration.size[2];
				}
			}
		}
		axis->specializationConstants.inputStride[4] = axis->specializationConstants.inputStride[3] * configuration.coordinateFeatures;
		axis->specializationConstants.outputStride[4] = axis->specializationConstants.outputStride[3] * configuration.coordinateFeatures;

		axis->specializationConstants.inverse = inverse;
		axis->specializationConstants.zeropad[0] = configuration.performZeropadding[axis_id];
		if (axis_id == 0)
			axis->specializationConstants.zeropad[1] = configuration.performZeropadding[axis_id + 1];
		else
			axis->specializationConstants.zeropad[1] = false;
		//not needed anymore as we don't transpose
		if (!inverse) {
			switch (axis_id) {
			case 0:
				axis->specializationConstants.ratio[0] = 1;
				axis->specializationConstants.ratioDirection[0] = false;
				if (configuration.FFTdim > 1) {
					if (configuration.performR2C) {
						axis->specializationConstants.ratio[1] = (configuration.size[0] / configuration.size[1] / 2 >= 1) ? configuration.size[0] / configuration.size[1] / 2 : 2 * configuration.size[1] / configuration.size[0];
						axis->specializationConstants.ratioDirection[1] = (configuration.size[0] / configuration.size[1] / 2 >= 1) ? true : false;
					}
					else {
						axis->specializationConstants.ratio[1] = (configuration.size[0] / configuration.size[1] >= 1) ? configuration.size[0] / configuration.size[1] : configuration.size[1] / configuration.size[0];
						axis->specializationConstants.ratioDirection[1] = (configuration.size[0] / configuration.size[1] >= 1) ? true : false;

					}
				}
				if (!configuration.performTranspose[0]) {
					axis->specializationConstants.ratioDirection[0] = false;
					axis->specializationConstants.ratioDirection[1] = true;
				}
				break;
			case 1:
				if (configuration.performR2C) {
					if (configuration.size[0] / configuration.size[1] / 2 >= 1) {
						axis->specializationConstants.ratio[0] = configuration.size[0] / configuration.size[1] / 2;
						axis->specializationConstants.ratioDirection[0] = true;
					}
					else
					{
						axis->specializationConstants.ratio[0] = (configuration.size[1] * 2 / configuration.size[0]);
						axis->specializationConstants.ratioDirection[0] = false;
					}
				}
				else {
					if (configuration.size[0] / configuration.size[1] >= 1) {
						axis->specializationConstants.ratio[0] = configuration.size[0] / configuration.size[1];
						axis->specializationConstants.ratioDirection[0] = true;
					}
					else {
						axis->specializationConstants.ratio[0] = configuration.size[1] / configuration.size[0];
						axis->specializationConstants.ratioDirection[0] = false;
					}
				}
				if ((configuration.performConvolution) && (configuration.FFTdim == 2)) {
					if (configuration.performR2C) {
						if (configuration.size[0] / configuration.size[1] / 2 >= 1) {
							axis->specializationConstants.ratio[1] = configuration.size[0] / configuration.size[1] / 2;
							axis->specializationConstants.ratioDirection[1] = false;
						}
						else
						{
							axis->specializationConstants.ratio[1] = (configuration.size[1] * 2 / configuration.size[0]);
							axis->specializationConstants.ratioDirection[1] = true;
						}
					}
					else {
						if (configuration.size[0] / configuration.size[1] >= 1) {
							axis->specializationConstants.ratio[1] = configuration.size[0] / configuration.size[1];
							axis->specializationConstants.ratioDirection[1] = false;
						}
						else {
							axis->specializationConstants.ratio[1] = configuration.size[1] / configuration.size[0];
							axis->specializationConstants.ratioDirection[1] = true;
						}
					}
				}
				if (configuration.FFTdim > 2) {
					if (configuration.size[1] / configuration.size[2] >= 1) {
						axis->specializationConstants.ratio[1] = configuration.size[1] / configuration.size[2];
						axis->specializationConstants.ratioDirection[1] = true;
					}
					else
					{
						axis->specializationConstants.ratio[1] = (configuration.size[2] / configuration.size[1]);
						axis->specializationConstants.ratioDirection[1] = false;
					}
				}
				if (!configuration.performTranspose[0]) {
					axis->specializationConstants.ratioDirection[0] = false;
				}
				if ((!configuration.performTranspose[1]) && (!((configuration.performConvolution) && (configuration.FFTdim == 2)))) {
					axis->specializationConstants.ratioDirection[1] = true;
				}
				break;
			case 2:
				if (configuration.size[1] / configuration.size[2] >= 1) {
					axis->specializationConstants.ratio[0] = configuration.size[1] / configuration.size[2];
					axis->specializationConstants.ratioDirection[0] = true;
				}
				else {
					axis->specializationConstants.ratio[0] = configuration.size[2] / configuration.size[1];
					axis->specializationConstants.ratioDirection[0] = false;
				}
				axis->specializationConstants.ratio[1] = 1;
				axis->specializationConstants.ratioDirection[1] = true;
				if ((configuration.performConvolution) && (configuration.FFTdim == 3)) {
					if (configuration.size[1] / configuration.size[2] >= 1) {
						axis->specializationConstants.ratio[1] = configuration.size[1] / configuration.size[2];
						axis->specializationConstants.ratioDirection[1] = false;
					}
					else {
						axis->specializationConstants.ratio[1] = configuration.size[2] / configuration.size[1];
						axis->specializationConstants.ratioDirection[1] = true;
					}
				}
				if (!configuration.performTranspose[1]) {
					axis->specializationConstants.ratioDirection[0] = false;
					axis->specializationConstants.ratioDirection[1] = true;
				}

				break;
			}
		}
		else {
			switch (axis_id) {
			case 0:
				axis->specializationConstants.ratio[1] = 1;
				axis->specializationConstants.ratioDirection[1] = true;
				if (configuration.FFTdim > 1) {
					if (configuration.performR2C) {
						axis->specializationConstants.ratio[0] = (configuration.size[0] / configuration.size[1] / 2 >= 1) ? configuration.size[0] / configuration.size[1] / 2 : 2 * configuration.size[1] / configuration.size[0];
						axis->specializationConstants.ratioDirection[0] = (configuration.size[0] / configuration.size[1] / 2 >= 1) ? false : true;
					}
					else
					{
						axis->specializationConstants.ratio[0] = (configuration.size[0] / configuration.size[1] >= 1) ? configuration.size[0] / configuration.size[1] : configuration.size[1] / configuration.size[0];
						axis->specializationConstants.ratioDirection[0] = (configuration.size[0] / configuration.size[1] >= 1) ? false : true;

					}
				}
				if (!configuration.performTranspose[0]) {
					axis->specializationConstants.ratioDirection[0] = false;
					axis->specializationConstants.ratioDirection[1] = true;
				}
				break;
			case 1:
				if (configuration.performR2C) {
					if (configuration.size[0] / configuration.size[1] / 2 >= 1) {
						axis->specializationConstants.ratio[1] = configuration.size[0] / configuration.size[1] / 2;
						axis->specializationConstants.ratioDirection[1] = false;
					}
					else
					{
						axis->specializationConstants.ratio[1] = (configuration.size[1] * 2 / configuration.size[0]);
						axis->specializationConstants.ratioDirection[1] = true;
					}
				}
				else {
					if (configuration.size[0] / configuration.size[1] >= 1) {
						axis->specializationConstants.ratio[1] = configuration.size[0] / configuration.size[1];
						axis->specializationConstants.ratioDirection[1] = false;
					}
					else {
						axis->specializationConstants.ratio[1] = configuration.size[1] / configuration.size[0];
						axis->specializationConstants.ratioDirection[1] = true;
					}
				}
				if (configuration.FFTdim > 2) {
					if (configuration.size[1] / configuration.size[2] >= 1) {
						axis->specializationConstants.ratio[0] = configuration.size[1] / configuration.size[2];
						axis->specializationConstants.ratioDirection[0] = false;
					}
					else
					{
						axis->specializationConstants.ratio[0] = (configuration.size[2] / configuration.size[1]);
						axis->specializationConstants.ratioDirection[0] = true;
					}
				}
				if (!configuration.performTranspose[0]) {
					axis->specializationConstants.ratioDirection[1] = true;
				}
				if (!configuration.performTranspose[1]) {
					axis->specializationConstants.ratioDirection[0] = false;
				}
				break;
			case 2:
				if (configuration.size[1] / configuration.size[2] >= 1) {
					axis->specializationConstants.ratio[1] = configuration.size[1] / configuration.size[2];
					axis->specializationConstants.ratioDirection[1] = false;
				}
				else {
					axis->specializationConstants.ratio[1] = configuration.size[2] / configuration.size[1];
					axis->specializationConstants.ratioDirection[1] = true;
				}
				axis->specializationConstants.ratio[0] = 1;
				axis->specializationConstants.ratioDirection[0] = false;
				if (!configuration.performTranspose[1]) {
					axis->specializationConstants.ratioDirection[1] = true;
				}
				break;
			}
		}
		axis->specializationConstants.inputOffset = 0;
		axis->specializationConstants.outputOffset = 0;

		VkDescriptorPoolSize descriptorPoolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
		descriptorPoolSize.descriptorCount = 2;
		if ((axis_id == 0) && (axis_upload_id == 0) && (configuration.FFTdim == 1) && (configuration.performConvolution))
			descriptorPoolSize.descriptorCount = 3;
		if ((axis_id == 1) && (axis_upload_id == 0) && (configuration.FFTdim == 2) && (configuration.performConvolution))
			descriptorPoolSize.descriptorCount = 3;
		if ((axis_id == 2) && (axis_upload_id == 0) && (configuration.FFTdim == 3) && (configuration.performConvolution))
			descriptorPoolSize.descriptorCount = 3;

		VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		descriptorPoolCreateInfo.poolSizeCount = 1;
		descriptorPoolCreateInfo.pPoolSizes = &descriptorPoolSize;
		descriptorPoolCreateInfo.maxSets = 1;
		vkCreateDescriptorPool(configuration.device[0], &descriptorPoolCreateInfo, NULL, &axis->descriptorPool);

		const VkDescriptorType descriptorType[3] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
		VkDescriptorSetLayoutBinding* descriptorSetLayoutBindings;
		descriptorSetLayoutBindings = (VkDescriptorSetLayoutBinding*)malloc(descriptorPoolSize.descriptorCount * sizeof(VkDescriptorSetLayoutBinding));
		for (uint32_t i = 0; i < descriptorPoolSize.descriptorCount; ++i) {
			descriptorSetLayoutBindings[i].binding = i;
			descriptorSetLayoutBindings[i].descriptorType = descriptorType[i];
			descriptorSetLayoutBindings[i].descriptorCount = 1;
			descriptorSetLayoutBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		}

		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		descriptorSetLayoutCreateInfo.bindingCount = descriptorPoolSize.descriptorCount;
		descriptorSetLayoutCreateInfo.pBindings = descriptorSetLayoutBindings;

		vkCreateDescriptorSetLayout(configuration.device[0], &descriptorSetLayoutCreateInfo, NULL, &axis->descriptorSetLayout);
		free(descriptorSetLayoutBindings);
		VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		descriptorSetAllocateInfo.descriptorPool = axis->descriptorPool;
		descriptorSetAllocateInfo.descriptorSetCount = 1;
		descriptorSetAllocateInfo.pSetLayouts = &axis->descriptorSetLayout;
		vkAllocateDescriptorSets(configuration.device[0], &descriptorSetAllocateInfo, &axis->descriptorSet);
		for (uint32_t i = 0; i < descriptorPoolSize.descriptorCount; ++i) {
			VkDescriptorBufferInfo descriptorBufferInfo = {};

			if (i == 0) {
				if ((axis_upload_id == FFTPlan->numAxisUploads[axis_id]-1) && (configuration.isInputFormatted) && (
					((axis_id == 0) && (!inverse) )
					|| ((axis_id == configuration.FFTdim-1) && (inverse) && (!configuration.performConvolution)))
					) {
					descriptorBufferInfo.buffer = configuration.inputBuffer[0];
					descriptorBufferInfo.range = configuration.inputBufferSize[0];
				}
				else {
					if ((axis_upload_id == 0) && (configuration.numberKernels > 1) && (inverse) && (!configuration.performConvolution)) {
						descriptorBufferInfo.buffer = configuration.outputBuffer[0];
						descriptorBufferInfo.range = configuration.outputBufferSize[0];
					}
					else {
						descriptorBufferInfo.buffer = configuration.buffer[0];
						descriptorBufferInfo.range = configuration.bufferSize[0];
					}
				}
				descriptorBufferInfo.offset = 0;
			}
			if (i == 1) {
				if ((axis_upload_id == 0) && (configuration.isOutputFormatted && (
					((axis_id == 0) && (inverse))
					|| ((axis_id == configuration.FFTdim-1) && (!inverse) && (!configuration.performConvolution))
					|| ((axis_id == 0) && (configuration.performConvolution) && (configuration.FFTdim == 1)))
					) ||
					((configuration.numberKernels > 1) && (
						(inverse)
						|| (axis_id == configuration.FFTdim-1)))
					) {
					descriptorBufferInfo.buffer = configuration.outputBuffer[0];
					descriptorBufferInfo.range = configuration.outputBufferSize[0];
				}
				else {
					descriptorBufferInfo.buffer = configuration.buffer[0];
					descriptorBufferInfo.range = configuration.bufferSize[0];
				}
				descriptorBufferInfo.offset = 0;
			}
			if (i == 2) {
				descriptorBufferInfo.buffer = configuration.kernel[0];
				descriptorBufferInfo.offset = 0;
				descriptorBufferInfo.range = configuration.kernelSize[0];
			}
			VkWriteDescriptorSet writeDescriptorSet = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			writeDescriptorSet.dstSet = axis->descriptorSet;
			writeDescriptorSet.dstBinding = i;
			writeDescriptorSet.dstArrayElement = 0;
			writeDescriptorSet.descriptorType = descriptorType[i];
			writeDescriptorSet.descriptorCount = 1;
			writeDescriptorSet.pBufferInfo = &descriptorBufferInfo;
			vkUpdateDescriptorSets(configuration.device[0], 1, &writeDescriptorSet, 0, NULL);

		}

		{
			VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			pipelineLayoutCreateInfo.setLayoutCount = 1;
			pipelineLayoutCreateInfo.pSetLayouts = &axis->descriptorSetLayout;

			VkPushConstantRange pushConstantRange = { VK_SHADER_STAGE_COMPUTE_BIT };
			pushConstantRange.offset = 0;
			pushConstantRange.size = sizeof(VkFFTPushConstantsLayout);
			// Push constant ranges are part of the pipeline layout
			pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
			pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;

			vkCreatePipelineLayout(configuration.device[0], &pipelineLayoutCreateInfo, NULL, &axis->pipelineLayout);
			if (!inverse) {
				if (axis_id == 0) {
					
					if (axis_upload_id == 0) {
						axis->axisBlock[0] = (axis->specializationConstants.fftDim / 8 > 1) ? axis->specializationConstants.fftDim / 8 : 1;
						if (axis->axisBlock[0] > 512) axis->axisBlock[0] = 512;

						axis->axisBlock[1] = 1;
						axis->axisBlock[2] = 1;
						axis->axisBlock[3] = axis->specializationConstants.fftDim;
					}
					else {
						axis->axisBlock[1] = (axis->specializationConstants.fftDim / 8 > 1) ? axis->specializationConstants.fftDim / 8 : 1;

						axis->axisBlock[0] = (axis->specializationConstants.stageStartSize > axis->groupedBatch) ? axis->groupedBatch : axis->specializationConstants.stageStartSize;

						axis->axisBlock[2] = 1;
						axis->axisBlock[3] = axis->specializationConstants.fftDim;
					}

				}
				if (axis_id == 1) {
					
					axis->axisBlock[1] = (axis->specializationConstants.fftDim / 8 > 1) ? axis->specializationConstants.fftDim / 8 : 1;

					if (configuration.performR2C) {
						if (axis_upload_id == 0) {
							for (uint32_t i = 0; i < 8; i++)
								VkFFTPlanSupportAxis(FFTPlan, 1, i, inverse);
						}
						axis->axisBlock[0] = (configuration.size[0] / 2 > axis->groupedBatch) ? axis->groupedBatch : configuration.size[0] / 2;
						/*if (axis->axisBlock[0] * axis->axisBlock[1] < 64)
							if (configuration.size[0]/2 > 64 / axis->axisBlock[1])
								axis->axisBlock[0] = 64 / axis->axisBlock[1];
							else
								axis->axisBlock[0] = configuration.size[0]/2;*/
					}
					else {
						axis->axisBlock[0] = (configuration.size[0] > axis->groupedBatch) ? axis->groupedBatch : configuration.size[0];
						/*if (axis->axisBlock[0] * axis->axisBlock[1] < 64)
							if (configuration.size[0] > 64 / axis->axisBlock[1])
								axis->axisBlock[0] = 64 / axis->axisBlock[1];
							else
								axis->axisBlock[0] = configuration.size[0];*/
					}
					
					axis->axisBlock[2] = 1;
					axis->axisBlock[3] = axis->specializationConstants.fftDim;
					
				}
				if (axis_id == 2) {
					axis->axisBlock[1] = (axis->specializationConstants.fftDim / 8 > 1) ? axis->specializationConstants.fftDim / 8 : 1;

					if (configuration.performR2C) {
						if (axis_upload_id == 0) {
							for (uint32_t i = 0; i < 8; i++)
								VkFFTPlanSupportAxis(FFTPlan, 2, i, inverse);
						}
						axis->axisBlock[0] = (configuration.size[0] / 2 > axis->groupedBatch) ? axis->groupedBatch : configuration.size[0] / 2;
						/*if (axis->axisBlock[0] * axis->axisBlock[1] < 64)
							if (configuration.size[0] / 2 > 64 / axis->axisBlock[1])
								axis->axisBlock[0] = 64 / axis->axisBlock[1];
							else
								axis->axisBlock[0] = configuration.size[0] / 2;*/
					}
					else {
						axis->axisBlock[0] = (configuration.size[0] > axis->groupedBatch) ? axis->groupedBatch : configuration.size[0];
						/*if (axis->axisBlock[0] * axis->axisBlock[1] < 64)
							if (configuration.size[0] > 64 / axis->axisBlock[1])
								axis->axisBlock[0] = 64 / axis->axisBlock[1];
							else
								axis->axisBlock[0] = configuration.size[0];*/
					}
					axis->axisBlock[2] = 1;
					axis->axisBlock[3] = axis->specializationConstants.fftDim;
				}
			}
			else {
				if (axis_id == 0) {
					if (axis_upload_id == 0) {
						axis->axisBlock[0] = (axis->specializationConstants.fftDim / 8 > 1) ? axis->specializationConstants.fftDim / 8 : 1;
						if (axis->axisBlock[0] > 512) axis->axisBlock[0] = 512;

						axis->axisBlock[1] = 1;
						axis->axisBlock[2] = 1;
						axis->axisBlock[3] = axis->specializationConstants.fftDim;
					}
					else {
						axis->axisBlock[1] = (axis->specializationConstants.fftDim / 8 > 1) ? axis->specializationConstants.fftDim / 8 : 1;

						axis->axisBlock[0] = (axis->specializationConstants.stageStartSize > axis->groupedBatch) ? axis->groupedBatch : axis->specializationConstants.stageStartSize;

						axis->axisBlock[2] = 1;
						axis->axisBlock[3] = axis->specializationConstants.fftDim;
					}
				}
				if (axis_id == 1) {
					
					axis->axisBlock[1] = (axis->specializationConstants.fftDim / 8 > 1) ? axis->specializationConstants.fftDim / 8 : 1;

					if (configuration.performR2C) {
						if (axis_upload_id == 0) {
							for (uint32_t i = 0; i < 8; i++)
								VkFFTPlanSupportAxis(FFTPlan, 1, i, inverse);
						}
						axis->axisBlock[0] = (configuration.size[0] / 2 > axis->groupedBatch) ? axis->groupedBatch : configuration.size[0] / 2;
						/*if (axis->axisBlock[0] * axis->axisBlock[1] < 64)
							if (configuration.size[0] / 2 > 64 / axis->axisBlock[1])
								axis->axisBlock[0] = 64 / axis->axisBlock[1];
							else
								axis->axisBlock[0] = configuration.size[0] / 2;*/
					}
					else {
						axis->axisBlock[0] = (configuration.size[0] > axis->groupedBatch) ? axis->groupedBatch : configuration.size[0];
						/*if (axis->axisBlock[0] * axis->axisBlock[1] < 64)
							if (configuration.size[0] > 64 / axis->axisBlock[1])
								axis->axisBlock[0] = 64 / axis->axisBlock[1];
							else
								axis->axisBlock[0] = configuration.size[0];*/
					}
					axis->axisBlock[2] = 1;
					axis->axisBlock[3] = axis->specializationConstants.fftDim;

				}
				if (axis_id == 2) {
					
					axis->axisBlock[1] = (axis->specializationConstants.fftDim / 8 > 1) ? axis->specializationConstants.fftDim / 8 : 1;

					if (configuration.performR2C) {
						if (axis_upload_id == 0) {
							for (uint32_t i = 0; i < 8; i++)
								VkFFTPlanSupportAxis(FFTPlan, 2, i, inverse);
						}
						axis->axisBlock[0] = (configuration.size[0] / 2 > axis->groupedBatch) ? axis->groupedBatch : configuration.size[0] / 2;
						/*if (axis->axisBlock[0] * axis->axisBlock[1] < 64)
							if (configuration.size[0] / 2 > 64 / axis->axisBlock[1])
								axis->axisBlock[0] = 64 / axis->axisBlock[1];
							else
								axis->axisBlock[0] = configuration.size[0] / 2;*/
					}
					else {
						axis->axisBlock[0] = (configuration.size[0] > axis->groupedBatch) ? axis->groupedBatch : configuration.size[0];
						/*if (axis->axisBlock[0] * axis->axisBlock[1] < 64)
							if (configuration.size[0] > 64 / axis->axisBlock[1])
								axis->axisBlock[0] = 64 / axis->axisBlock[1];
							else
								axis->axisBlock[0] = configuration.size[0];*/
					}
					axis->axisBlock[2] = 1;
					axis->axisBlock[3] = axis->specializationConstants.fftDim;

				}

			}
			VkSpecializationMapEntry specializationMapEntries[30] = { {} };
			for (uint32_t i = 0; i < 30; i++) {
				specializationMapEntries[i].constantID = i + 1;
				specializationMapEntries[i].size = sizeof(uint32_t);
				specializationMapEntries[i].offset = i * sizeof(uint32_t);
			}
			VkSpecializationInfo specializationInfo = {};
			specializationInfo.dataSize = 30 * sizeof(uint32_t);
			specializationInfo.mapEntryCount = 30;
			specializationInfo.pMapEntries = specializationMapEntries;
			axis->specializationConstants.localSize[0] = axis->axisBlock[0];
			axis->specializationConstants.localSize[1] = axis->axisBlock[1];
			axis->specializationConstants.localSize[2] = axis->axisBlock[2];
			specializationInfo.pData = &axis->specializationConstants;
			VkPipelineShaderStageCreateInfo pipelineShaderStageCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };

			VkComputePipelineCreateInfo computePipelineCreateInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };


			pipelineShaderStageCreateInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			if (configuration.performR2C) {
				if (axis_id == 0) {
					if (inverse) {
						switch (configuration.registerBoost) {
						case 1:
						{
							VkFFTInitShader(1, &pipelineShaderStageCreateInfo.module);
							break;
						}
						case 2:
						{
							switch (axis->specializationConstants.fftDim) {
							case 8192:
								VkFFTInitShader(23, &pipelineShaderStageCreateInfo.module);
								break;
							default:
								VkFFTInitShader(1, &pipelineShaderStageCreateInfo.module);
								break;
							}
							break;
						}
						case 4:
						{
							switch (axis->specializationConstants.fftDim) {
							case 8192:
								VkFFTInitShader(23, &pipelineShaderStageCreateInfo.module);
								break;
							case 16384:
								VkFFTInitShader(33, &pipelineShaderStageCreateInfo.module);
								break;
							default:
								VkFFTInitShader(1, &pipelineShaderStageCreateInfo.module);
								break;
							}
							break;
						}
						}
					}
					else {
						switch (configuration.registerBoost) {
						case 1:
						{
							VkFFTInitShader(3, &pipelineShaderStageCreateInfo.module);
							break;
						}
						case 2:
						{
							switch (axis->specializationConstants.fftDim) {
							case 8192:
								VkFFTInitShader(24, &pipelineShaderStageCreateInfo.module);
								break;
							default:
								VkFFTInitShader(3, &pipelineShaderStageCreateInfo.module);
								break;
							}
							break;
						}
						case 4:
						{
							switch (axis->specializationConstants.fftDim) {
							case 8192:
								VkFFTInitShader(24, &pipelineShaderStageCreateInfo.module);
								break;
							case 16384:
								VkFFTInitShader(34, &pipelineShaderStageCreateInfo.module);
								break;
							default:
								VkFFTInitShader(3, &pipelineShaderStageCreateInfo.module);
								break;
							}
							break;
						}
						}

					}
				}
				if (axis_id == 1) {

					if ((configuration.FFTdim == 2) && (configuration.performConvolution)&&(axis_upload_id == 0)) {
						

							switch (configuration.matrixConvolution) {
							case 1:
								VkFFTInitShader(8, &pipelineShaderStageCreateInfo.module);
								break;
							case 2:
								if (configuration.symmetricKernel)
									VkFFTInitShader(11, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(14, &pipelineShaderStageCreateInfo.module);
								break;
							case 3:
								if (configuration.symmetricKernel)
									VkFFTInitShader(17, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(20, &pipelineShaderStageCreateInfo.module);
								break;
							}
						
					}
					else {
						VkFFTInitShader(7, &pipelineShaderStageCreateInfo.module);
					}

				}

				if (axis_id == 2) {
					if ((configuration.FFTdim == 3) && (configuration.performConvolution)&&(axis_upload_id == 0)) {
						
							switch (configuration.matrixConvolution) {
							case 1:
								VkFFTInitShader(8, &pipelineShaderStageCreateInfo.module);
								break;
							case 2:
								if (configuration.symmetricKernel)
									VkFFTInitShader(11, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(14, &pipelineShaderStageCreateInfo.module);
								break;
							case 3:
								if (configuration.symmetricKernel)
									VkFFTInitShader(17, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(20, &pipelineShaderStageCreateInfo.module);
								break;
							}
						
					}
					else {
						
							VkFFTInitShader(7, &pipelineShaderStageCreateInfo.module);
					}
				}
			}
			else {
				if (axis_id == 0) {
					if ((configuration.FFTdim == 1) && (configuration.performConvolution) && (axis_upload_id == 0)) {
						if (axis_upload_id == 0) {
							switch (configuration.matrixConvolution) {
							case 1:
								VkFFTInitShader(9, &pipelineShaderStageCreateInfo.module);
								break;
							case 2:
								if (configuration.symmetricKernel)
									VkFFTInitShader(12, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(15, &pipelineShaderStageCreateInfo.module);
								break;
							case 3:
								if (configuration.symmetricKernel)
									VkFFTInitShader(18, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(21, &pipelineShaderStageCreateInfo.module);
								break;
							}
						}
						else {
							switch (configuration.matrixConvolution) {
							case 1:
								VkFFTInitShader(10, &pipelineShaderStageCreateInfo.module);
								break;
							case 2:
								if (configuration.symmetricKernel)
									VkFFTInitShader(13, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(16, &pipelineShaderStageCreateInfo.module);
								break;
							case 3:
								if (configuration.symmetricKernel)
									VkFFTInitShader(19, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(22, &pipelineShaderStageCreateInfo.module);
								break;
							}
						}
					}
					else {
						switch (configuration.registerBoost) {
						case 1:
						{
							if (axis_upload_id == 0)
								VkFFTInitShader(0, &pipelineShaderStageCreateInfo.module);
							else
								VkFFTInitShader(2, &pipelineShaderStageCreateInfo.module);
							break;
						}
						case 2:
						{
							switch (axis->specializationConstants.fftDim) {
							case 8192:
								VkFFTInitShader(25, &pipelineShaderStageCreateInfo.module);
								break;
							default:
								if (axis_upload_id == 0)
									VkFFTInitShader(0, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(2, &pipelineShaderStageCreateInfo.module);
								break;
							}
							break;
						}
						case 4:
						{
							switch (axis->specializationConstants.fftDim) {
							case 8192:
								VkFFTInitShader(25, &pipelineShaderStageCreateInfo.module);
								break;
							case 16384:
								VkFFTInitShader(35, &pipelineShaderStageCreateInfo.module);
								break;
							default:
								if (axis_upload_id == 0)
									VkFFTInitShader(0, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(2, &pipelineShaderStageCreateInfo.module);
								break;
							}
							break;
						}
						}
					}
				}
				if (axis_id == 1) {

					if ((configuration.FFTdim == 2) && (configuration.performConvolution) && (axis_upload_id == 0)) {
						
							switch (configuration.matrixConvolution) {
							case 1:
								VkFFTInitShader(8, &pipelineShaderStageCreateInfo.module);
								break;
							case 2:
								if (configuration.symmetricKernel)
									VkFFTInitShader(11, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(14, &pipelineShaderStageCreateInfo.module);
								break;
							case 3:
								if (configuration.symmetricKernel)
									VkFFTInitShader(17, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(20, &pipelineShaderStageCreateInfo.module);
								break;
							}
						
					}
					else {
							VkFFTInitShader(7, &pipelineShaderStageCreateInfo.module);
						
					}

				}

				if (axis_id == 2) {
					if ((configuration.FFTdim == 3) && (configuration.performConvolution) && (axis_upload_id == 0)) {
						
							switch (configuration.matrixConvolution) {
							case 1:
								VkFFTInitShader(8, &pipelineShaderStageCreateInfo.module);
								break;
							case 2:
								if (configuration.symmetricKernel)
									VkFFTInitShader(11, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(14, &pipelineShaderStageCreateInfo.module);
								break;
							case 3:
								if (configuration.symmetricKernel)
									VkFFTInitShader(17, &pipelineShaderStageCreateInfo.module);
								else
									VkFFTInitShader(20, &pipelineShaderStageCreateInfo.module);
								break;
							}
						
					}
					else {
						VkFFTInitShader(7, &pipelineShaderStageCreateInfo.module);
					}
				}
			}

			pipelineShaderStageCreateInfo.pName = "main";
			pipelineShaderStageCreateInfo.pSpecializationInfo = &specializationInfo;
			computePipelineCreateInfo.stage = pipelineShaderStageCreateInfo;
			computePipelineCreateInfo.layout = axis->pipelineLayout;



			vkCreateComputePipelines(configuration.device[0], VK_NULL_HANDLE, 1, &computePipelineCreateInfo, NULL, &axis->pipeline);
			vkDestroyShaderModule(configuration.device[0], pipelineShaderStageCreateInfo.module, NULL);
		}


	}
	void VkFFTPlanSupportAxis(VkFFTPlan* FFTPlan, uint32_t axis_id, uint32_t axis_upload_id, bool inverse) {
		//get radix stages
		VkFFTAxis* axis = &FFTPlan->supportAxes[axis_id - 1][axis_upload_id];
		if (axis_id == 1) {
			//configure radix stages
			uint32_t logSize = log2(configuration.size[axis_id]);
			uint32_t numPasses[8][8];//4096-8k(256KB)-16k(256KB)-32k-64k - find correct strided FFT configuration - x axis | 256-512-1024-2048(256KB)-4096(256KB)-8k(future?)-16k(future?) - find correct strided FFT configuration
			for (uint32_t i = 0; i < 8; i++) {
				for (uint32_t j = 0; j < 8; j++) {
					numPasses[i][j] = 0;
				}
			}
			uint32_t temp = configuration.size[axis_id];
			uint32_t startStage = 4096;
			uint32_t continueStage = 256;
			uint32_t maxPassId[2] = { 0,0 };
			uint32_t minPassId[2] = { 0,0 };
			maxPassId[0] += log2(configuration.registerBoost);
			uint32_t maxSingleSize = 8 * 4096 / configuration.coalescedMemory;
			maxPassId[1] = log2(maxSingleSize / 256);
			minPassId[1] = (maxSingleSize >= 512) ? 1 : 0;
			//maxPassId[1] += log2(configuration.registerBoost);//in development
			for (uint32_t i = 0; i < 8; i++) {
				for (uint32_t j = 0; j < 8; j++) {
					temp /= startStage;
					numPasses[i][j]++;
					while (temp > 1)
					{
						temp /= continueStage;
						numPasses[i][j]++;
					}
					continueStage *= 2;
					temp = configuration.size[axis_id];
				}
				continueStage = 256;
				startStage *= 2;
			}
			uint32_t passId[2] = { minPassId[0], minPassId[1] };
			for (uint32_t i = minPassId[0]; i < maxPassId[0]+1; i++) {
				for (uint32_t j = minPassId[1]; j < maxPassId[1]+1; j++) {
					if (numPasses[i][j] < numPasses[passId[0]][passId[1]]) {
						passId[0] = i;
						passId[1] = j;
					}
				}
			}
			FFTPlan->numSupportAxisUploads[axis_id - 1] = numPasses[passId[0]][passId[1]];
			if (axis_upload_id >= numPasses[passId[0]][passId[1]])
				return;
			if (axis_upload_id == 0) {
				//first pass is non-strided, special case
				switch (configuration.radix) {
				case 8: {
					uint32_t logSize0Pass = (12 + passId[0] < logSize) ? 12 + passId[0] : logSize; //4096 + shift
					if ((axis_upload_id + 1 == numPasses[passId[0]][passId[1]] - 1) && (logSize - logSize0Pass < 3))
						logSize0Pass -= (3 - (logSize - logSize0Pass));
					uint32_t stage8 = logSize0Pass / 3;
					uint32_t stage4 = 0;
					uint32_t stage2 = 0;
					if (logSize0Pass % 3 == 2)
						stage4 = 1;
					if (logSize0Pass % 3 == 1)
						stage2 = 1;
					uint32_t totNumStages = stage8 + stage4 + stage2;

					axis->specializationConstants.numStages = stage8;
					axis->specializationConstants.fftDim = pow(8, stage8);
					axis->specializationConstants.stageRadix[0] = 8;
					axis->specializationConstants.stageRadix[1] = 8;

					if (stage4 == 1) {
						axis->specializationConstants.numStages++;
						axis->specializationConstants.stageRadix[1] = 4;
						axis->specializationConstants.fftDim *= 4;
					}
					if (stage2 == 1) {
						axis->specializationConstants.numStages++;
						axis->specializationConstants.stageRadix[1] = 2;
						axis->specializationConstants.fftDim *= 2;
					}
					axis->specializationConstants.stageStartSize = 1;
					if (configuration.performR2C)
						axis->specializationConstants.fft_dim_x = configuration.size[0] / 2;
					else
						axis->specializationConstants.fft_dim_x = configuration.size[0];

					break;
				}
				case 4: {
					uint32_t stage4 = logSize / 2;
					uint32_t stage2 = 0;
					if (logSize % 2 == 1)
						stage2 = 1;
					axis->specializationConstants.numStages = stage4 + stage2;


					axis->specializationConstants.stageRadix[0] = 4;
					axis->specializationConstants.stageRadix[1] = 4;
					if (logSize % 2 == 1)
						axis->specializationConstants.stageRadix[1] = 2;
					break;
				}
				case 2: {
					uint32_t stage2 = logSize;

					axis->specializationConstants.numStages = stage2;


					axis->specializationConstants.stageRadix[0] = 2;
					axis->specializationConstants.stageRadix[1] = 2;
					break;
				}
				}
			}
			else {
				//passes after first are done similar to strided passes in y and z
				uint32_t logSizeLaterPass = (logSize - 12 - passId[0] < 3) ? 3 : logSize - 12 - passId[0]; //4096 + shift
				switch (configuration.radix) {
				case 8: {
					uint32_t stage8 = logSizeLaterPass / 3;
					uint32_t stage4 = 0;
					uint32_t stage2 = 0;
					if (logSizeLaterPass % 3 == 2)
						stage4 = 1;
					if (logSizeLaterPass % 3 == 1)
						stage2 = 1;
					uint32_t totNumStages = stage8 + stage4 + stage2;
					uint32_t locNumStages = 0;
					if (passId[1] == minPassId[1]) {
						locNumStages = stage8 / (numPasses[passId[0]][passId[1]] - 1);
						if (axis_upload_id < stage8 % (numPasses[passId[0]][passId[1]] - 1))
							locNumStages++;
						axis->specializationConstants.numStages = locNumStages;
						axis->specializationConstants.fftDim = pow(8, locNumStages);
						axis->specializationConstants.stageRadix[0] = 8;
						axis->specializationConstants.stageRadix[1] = 8;

						if (axis_upload_id == (numPasses[passId[0]][passId[1]] - 1)) {
							if (stage4 == 1) {
								axis->specializationConstants.numStages++;
								axis->specializationConstants.stageRadix[1] = 4;
								axis->specializationConstants.fftDim *= 4;
							}
							if (stage2 == 1) {
								axis->specializationConstants.numStages++;
								axis->specializationConstants.stageRadix[1] = 2;
								axis->specializationConstants.fftDim *= 2;
							}
						}
						axis->specializationConstants.stageStartSize = FFTPlan->supportAxes[axis_id-1][axis_upload_id - 1].specializationConstants.stageStartSize * FFTPlan->supportAxes[axis_id - 1][axis_upload_id - 1].specializationConstants.fftDim;
						axis->specializationConstants.fft_dim_x = configuration.size[1];
					}
					else {
						if (axis_upload_id < numPasses[passId[0]][passId[1]] - 1) {
							uint32_t locLogSize = 8 + passId[1];
							if ((axis_upload_id + 1 == numPasses[passId[0]][passId[1]] - 1) && (logSizeLaterPass - (8 + passId[1]) * (numPasses[passId[0]][passId[1]] - 2) < 3))
								locLogSize -= (3 - (logSizeLaterPass - (8 + passId[1]) * (numPasses[passId[0]][passId[1]] - 2)));
							uint32_t locStage8 = locLogSize / 3;
							uint32_t locStage4 = 0;
							uint32_t locStage2 = 0;
							if (locLogSize % 3 == 2)
								locStage4 = 1;
							if (locLogSize % 3 == 1)
								locStage2 = 1;
							axis->specializationConstants.numStages = locStage8 + locStage4 + locStage2;
							axis->specializationConstants.fftDim = pow(2, locLogSize);
							axis->specializationConstants.stageRadix[0] = 8;
							axis->specializationConstants.stageRadix[1] = 8;

							if (locStage4 == 1) {
								axis->specializationConstants.stageRadix[1] = 4;
							}
							if (locStage2 == 1) {
								axis->specializationConstants.stageRadix[1] = 2;
							}
							axis->specializationConstants.stageStartSize = FFTPlan->supportAxes[axis_id - 1][axis_upload_id - 1].specializationConstants.stageStartSize * FFTPlan->supportAxes[axis_id - 1][axis_upload_id - 1].specializationConstants.fftDim;
							if (configuration.performR2C)
								axis->specializationConstants.fft_dim_x = configuration.size[0] / 2;
							else
								axis->specializationConstants.fft_dim_x = configuration.size[0];
						}
						else {
							uint32_t locLogSize = (logSizeLaterPass - (8 + passId[1]) * (numPasses[passId[0]][passId[1]] - 2) < 3) ? 3 : logSizeLaterPass - (8 + passId[1]) * (numPasses[passId[0]][passId[1]] - 2);
							uint32_t locStage8 = locLogSize / 3;
							uint32_t locStage4 = 0;
							uint32_t locStage2 = 0;
							if (locLogSize % 3 == 2)
								locStage4 = 1;
							if (locLogSize % 3 == 1)
								locStage2 = 1;
							axis->specializationConstants.numStages = locStage8 + locStage4 + locStage2;
							axis->specializationConstants.fftDim = pow(2, locLogSize);
							axis->specializationConstants.stageRadix[0] = 8;
							axis->specializationConstants.stageRadix[1] = 8;

							if (locStage4 == 1) {
								axis->specializationConstants.stageRadix[1] = 4;
							}
							if (locStage2 == 1) {
								axis->specializationConstants.stageRadix[1] = 2;
							}
							axis->specializationConstants.stageStartSize = FFTPlan->supportAxes[axis_id - 1][axis_upload_id - 1].specializationConstants.stageStartSize * FFTPlan->supportAxes[axis_id - 1][axis_upload_id - 1].specializationConstants.fftDim;
							if (configuration.performR2C)
								axis->specializationConstants.fft_dim_x = configuration.size[0] / 2;
							else
								axis->specializationConstants.fft_dim_x = configuration.size[0];
						}
					}


					break;
				}
				case 4: {
					uint32_t stage4 = logSize / 2;
					uint32_t stage2 = 0;
					if (logSize % 2 == 1)
						stage2 = 1;
					axis->specializationConstants.numStages = stage4 + stage2;


					axis->specializationConstants.stageRadix[0] = 4;
					axis->specializationConstants.stageRadix[1] = 4;
					if (logSize % 2 == 1)
						axis->specializationConstants.stageRadix[1] = 2;
					break;
				}
				case 2: {
					uint32_t stage2 = logSize;

					axis->specializationConstants.numStages = stage2;


					axis->specializationConstants.stageRadix[0] = 2;
					axis->specializationConstants.stageRadix[1] = 2;
					break;
				}
				}
			}
		}
		else {
			//configure radix stages
			uint32_t logSize = log2(configuration.size[axis_id]);
			uint32_t numPasses[8] = { 0,0,0,0,0,0,0,0 };//256-512-1024-2048(256KB)-4096(256KB)-8k(future?)-16k(future?) - find correct strided FFT configuration
			uint32_t temp = configuration.size[axis_id];
			uint32_t startStage = 256;
			uint32_t maxSingleSize = 8 * 4096 / configuration.coalescedMemory;
			uint32_t maxPassId = log2(maxSingleSize / 256);
			uint32_t minPassId = (maxSingleSize >= 512) ? 1 : 0;
			//maxPassId += log2(configuration.registerBoost); //in development
			for (uint32_t i = 0; i < 8; i++) {
				while (temp > 1)
				{
					temp /= startStage;
					numPasses[i]++;
				}
				temp = configuration.size[axis_id];
				startStage *= 2;
			}
			uint32_t passId = minPassId;
			for (uint32_t i = minPassId; i < maxPassId+1; i++) {
				if (numPasses[i] < numPasses[passId]) {
					passId = i;
				}
			}
			FFTPlan->numSupportAxisUploads[axis_id-1] = numPasses[passId];
			if (axis_upload_id >= numPasses[passId])
				return;
			switch (configuration.radix) {
			case 8: {
				uint32_t stage8 = logSize / 3;
				uint32_t stage4 = 0;
				uint32_t stage2 = 0;
				if (logSize % 3 == 2)
					stage4 = 1;
				if (logSize % 3 == 1)
					stage2 = 1;
				uint32_t totNumStages = stage8 + stage4 + stage2;
				uint32_t locNumStages = 0;
				if (passId == minPassId) {
					locNumStages = stage8 / numPasses[passId];
					if (axis_upload_id < stage8 % numPasses[passId])
						locNumStages++;
					axis->specializationConstants.numStages = locNumStages;
					axis->specializationConstants.fftDim = pow(8, locNumStages);
					axis->specializationConstants.stageRadix[0] = 8;
					axis->specializationConstants.stageRadix[1] = 8;

					if (axis_upload_id == numPasses[passId] - 1) {
						if (stage4 == 1) {
							axis->specializationConstants.numStages++;
							axis->specializationConstants.stageRadix[1] = 4;
							axis->specializationConstants.fftDim *= 4;
						}
						if (stage2 == 1) {
							axis->specializationConstants.numStages++;
							axis->specializationConstants.stageRadix[1] = 2;
							axis->specializationConstants.fftDim *= 2;
						}
					}
					axis->specializationConstants.stageStartSize = (axis_upload_id == 0) ? 1 : FFTPlan->supportAxes[axis_id - 1][axis_upload_id - 1].specializationConstants.stageStartSize * FFTPlan->supportAxes[axis_id - 1][axis_upload_id - 1].specializationConstants.fftDim;
					axis->specializationConstants.fft_dim_x = configuration.size[1];
				}
				else {
					if (axis_upload_id < numPasses[passId] - 1) {

						uint32_t locLogSize = 8 + passId;
						if ((axis_upload_id + 1 == numPasses[passId] - 1) && (logSize - (8 + passId) * (numPasses[passId] - 1) < 3))
							locLogSize -= (3 - (logSize - (8 + passId) * (numPasses[passId] - 1)));
						uint32_t locStage8 = locLogSize / 3;
						uint32_t locStage4 = 0;
						uint32_t locStage2 = 0;
						if (locLogSize % 3 == 2)
							locStage4 = 1;
						if (locLogSize % 3 == 1)
							locStage2 = 1;
						axis->specializationConstants.numStages = locStage8 + locStage4 + locStage2;
						axis->specializationConstants.fftDim = pow(2, locLogSize);
						axis->specializationConstants.stageRadix[0] = 8;
						axis->specializationConstants.stageRadix[1] = 8;

						if (locStage4 == 1) {
							axis->specializationConstants.stageRadix[1] = 4;
						}
						if (locStage2 == 1) {
							axis->specializationConstants.stageRadix[1] = 2;
						}
						axis->specializationConstants.stageStartSize = (axis_upload_id == 0) ? 1 : FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.stageStartSize * FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.fftDim;
						if (configuration.performR2C)
							axis->specializationConstants.fft_dim_x = configuration.size[0] / 2;
						else
							axis->specializationConstants.fft_dim_x = configuration.size[0];
					}
					else {
						uint32_t locLogSize = (logSize - (8 + passId) * (numPasses[passId] - 1) < 3) ? 3 : logSize - (8 + passId) * (numPasses[passId] - 1);
						uint32_t locStage8 = locLogSize / 3;
						uint32_t locStage4 = 0;
						uint32_t locStage2 = 0;
						if (locLogSize % 3 == 2)
							locStage4 = 1;
						if (locLogSize % 3 == 1)
							locStage2 = 1;
						axis->specializationConstants.numStages = locStage8 + locStage4 + locStage2;
						axis->specializationConstants.fftDim = pow(2, locLogSize);
						axis->specializationConstants.stageRadix[0] = 8;
						axis->specializationConstants.stageRadix[1] = 8;

						if (locStage4 == 1) {
							axis->specializationConstants.stageRadix[1] = 4;
						}
						if (locStage2 == 1) {
							axis->specializationConstants.stageRadix[1] = 2;
						}
						axis->specializationConstants.stageStartSize = (axis_upload_id == 0) ? 1 : FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.stageStartSize * FFTPlan->axes[axis_id][axis_upload_id - 1].specializationConstants.fftDim;
						if (configuration.performR2C)
							axis->specializationConstants.fft_dim_x = configuration.size[0] / 2;
						else
							axis->specializationConstants.fft_dim_x = configuration.size[0];
					}
				}


				break;
			}
			case 4: {
				uint32_t stage4 = logSize / 2;
				uint32_t stage2 = 0;
				if (logSize % 2 == 1)
					stage2 = 1;
				axis->specializationConstants.numStages = stage4 + stage2;


				axis->specializationConstants.stageRadix[0] = 4;
				axis->specializationConstants.stageRadix[1] = 4;
				if (logSize % 2 == 1)
					axis->specializationConstants.stageRadix[1] = 2;
				break;
			}
			case 2: {
				uint32_t stage2 = logSize;

				axis->specializationConstants.numStages = stage2;


				axis->specializationConstants.stageRadix[0] = 2;
				axis->specializationConstants.stageRadix[1] = 2;
				break;
			}
			}
		}
		axis->specializationConstants.passID = FFTPlan->numSupportAxisUploads[axis_id - 1] - 1 - axis_upload_id;
		axis->specializationConstants.fft_dim_full = configuration.size[axis_id];
		axis->groupedBatch = (4096 / axis->specializationConstants.fftDim >= configuration.coalescedMemory / 8) ? 4096 / axis->specializationConstants.fftDim : configuration.coalescedMemory / 8;
		//axis->groupedBatch = ((axis_upload_id>0)&&(axis->groupedBatch > axis->specializationConstants.stageStartSize)) ? axis->specializationConstants.stageStartSize : axis->groupedBatch;
		//configure strides
		//perform r2c
		axis->specializationConstants.inputStride[0] = 1;
		axis->specializationConstants.inputStride[3] = (configuration.size[0] / 2 + 1) * configuration.size[1] * configuration.size[2];

		if (axis_id == 1)
		{

			//don't transpose 0-1
			axis->specializationConstants.inputStride[1] = configuration.size[1];
			axis->specializationConstants.inputStride[2] = (configuration.size[0] / 2 + 1) * configuration.size[1];
			axis->specializationConstants.inputStride[3] = (configuration.size[0] / 2 + 1) * configuration.size[1] * configuration.size[2];
		}
		if (axis_id == 2)
		{

			//don't transpose 0-1, don't transpose 1-2
			axis->specializationConstants.inputStride[1] = (configuration.size[0] / 2 + 1) * configuration.size[1];
			axis->specializationConstants.inputStride[2] = configuration.size[1];

		}

		axis->specializationConstants.outputStride[0] = axis->specializationConstants.inputStride[0];
		axis->specializationConstants.outputStride[1] = axis->specializationConstants.inputStride[1];
		axis->specializationConstants.outputStride[2] = axis->specializationConstants.inputStride[2];
		axis->specializationConstants.outputStride[3] = axis->specializationConstants.inputStride[3];

		axis->specializationConstants.inputStride[4] = axis->specializationConstants.inputStride[3] * configuration.coordinateFeatures;
		axis->specializationConstants.outputStride[4] = axis->specializationConstants.outputStride[3] * configuration.coordinateFeatures;

		axis->specializationConstants.inverse = inverse;
		axis->specializationConstants.zeropad[0] = configuration.performZeropadding[axis_id];
		axis->specializationConstants.zeropad[1] = false;
		axis->specializationConstants.ratio[0] = configuration.size[axis_id - 1] / configuration.size[axis_id];
		axis->specializationConstants.ratio[1] = configuration.size[axis_id - 1] / configuration.size[axis_id];
		axis->specializationConstants.ratioDirection[0] = false;
		axis->specializationConstants.ratioDirection[1] = true;
		axis->specializationConstants.inputOffset = configuration.size[0] * configuration.size[1] / 2;
		axis->specializationConstants.outputOffset = configuration.size[0] * configuration.size[1] / 2;

		VkDescriptorPoolSize descriptorPoolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
		descriptorPoolSize.descriptorCount = 2;
		if ((axis_id == 1) && (axis_upload_id == 0) && (configuration.FFTdim == 2) && (configuration.performConvolution))
			descriptorPoolSize.descriptorCount = 3;
		if ((axis_id == 2) && (axis_upload_id == 0) && (configuration.FFTdim == 3) && (configuration.performConvolution))
			descriptorPoolSize.descriptorCount = 3;

		VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		descriptorPoolCreateInfo.poolSizeCount = 1;
		descriptorPoolCreateInfo.pPoolSizes = &descriptorPoolSize;
		descriptorPoolCreateInfo.maxSets = 1;
		vkCreateDescriptorPool(configuration.device[0], &descriptorPoolCreateInfo, NULL, &axis->descriptorPool);

		const VkDescriptorType descriptorType[3] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
		VkDescriptorSetLayoutBinding* descriptorSetLayoutBindings;
		descriptorSetLayoutBindings = (VkDescriptorSetLayoutBinding*)malloc(descriptorPoolSize.descriptorCount * sizeof(VkDescriptorSetLayoutBinding));
		for (uint32_t i = 0; i < descriptorPoolSize.descriptorCount; ++i) {
			descriptorSetLayoutBindings[i].binding = i;
			descriptorSetLayoutBindings[i].descriptorType = descriptorType[i];
			descriptorSetLayoutBindings[i].descriptorCount = 1;
			descriptorSetLayoutBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		}

		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		descriptorSetLayoutCreateInfo.bindingCount = descriptorPoolSize.descriptorCount;
		descriptorSetLayoutCreateInfo.pBindings = descriptorSetLayoutBindings;

		vkCreateDescriptorSetLayout(configuration.device[0], &descriptorSetLayoutCreateInfo, NULL, &axis->descriptorSetLayout);
		free(descriptorSetLayoutBindings);
		VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		descriptorSetAllocateInfo.descriptorPool = axis->descriptorPool;
		descriptorSetAllocateInfo.descriptorSetCount = 1;
		descriptorSetAllocateInfo.pSetLayouts = &axis->descriptorSetLayout;
		vkAllocateDescriptorSets(configuration.device[0], &descriptorSetAllocateInfo, &axis->descriptorSet);
		for (uint32_t i = 0; i < descriptorPoolSize.descriptorCount; ++i) {
			VkDescriptorBufferInfo descriptorBufferInfo = {};

			if (i == 0) {
				descriptorBufferInfo.buffer = configuration.buffer[0];
				descriptorBufferInfo.range = configuration.bufferSize[0];
				/*if (configuration.isInputFormatted && (
						((axis_id == 0) && (!inverse)) 
						|| ((axis_id == configuration.FFTdim-1) && (inverse)))
					) {
					descriptorBufferInfo.buffer = configuration.inputBuffer[0];
					descriptorBufferInfo.range = configuration.inputBufferSize[0];
				}
				else {
					if ((configuration.numberKernels > 1) && (inverse)) {
						descriptorBufferInfo.buffer = configuration.outputBuffer[0];
						descriptorBufferInfo.range = configuration.outputBufferSize[0];
					}
					else {
						descriptorBufferInfo.buffer = configuration.buffer[0];
						descriptorBufferInfo.range = configuration.bufferSize[0];
					}
				}*/
				descriptorBufferInfo.offset = 0;
			}
			if (i == 1) {
				descriptorBufferInfo.buffer = configuration.buffer[0];
				descriptorBufferInfo.range = configuration.bufferSize[0];
				/*if ((configuration.isOutputFormatted && (
						((axis_id == 0) && (inverse))
						|| ((axis_id == configuration.FFTdim-1) && (!inverse) && (!configuration.performConvolution))
						|| ((axis_id == 0) && (configuration.performConvolution) && (configuration.FFTdim == 1)))
					)||
					((configuration.numberKernels>1)&&(
						(inverse)
						||(axis_id== configuration.FFTdim-1)))
					) {
					descriptorBufferInfo.buffer = configuration.outputBuffer[0];
					descriptorBufferInfo.range = configuration.outputBufferSize[0];
				}
				else {
					descriptorBufferInfo.buffer = configuration.buffer[0];
					descriptorBufferInfo.range = configuration.bufferSize[0];
				}*/
				descriptorBufferInfo.offset = 0;
			}
			if (i == 2) {
				descriptorBufferInfo.buffer = configuration.kernel[0];
				descriptorBufferInfo.offset = 0;
				descriptorBufferInfo.range = configuration.kernelSize[0];
			}
			VkWriteDescriptorSet writeDescriptorSet = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			writeDescriptorSet.dstSet = axis->descriptorSet;
			writeDescriptorSet.dstBinding = i;
			writeDescriptorSet.dstArrayElement = 0;
			writeDescriptorSet.descriptorType = descriptorType[i];
			writeDescriptorSet.descriptorCount = 1;
			writeDescriptorSet.pBufferInfo = &descriptorBufferInfo;
			vkUpdateDescriptorSets(configuration.device[0], 1, &writeDescriptorSet, 0, NULL);

		}

		{
			VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			pipelineLayoutCreateInfo.setLayoutCount = 1;
			pipelineLayoutCreateInfo.pSetLayouts = &axis->descriptorSetLayout;

			VkPushConstantRange pushConstantRange = { VK_SHADER_STAGE_COMPUTE_BIT };
			pushConstantRange.offset = 0;
			pushConstantRange.size = sizeof(VkFFTPushConstantsLayout);
			// Push constant ranges are part of the pipeline layout
			pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
			pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;


			vkCreatePipelineLayout(configuration.device[0], &pipelineLayoutCreateInfo, NULL, &axis->pipelineLayout);
			if (axis_id == 1) {
				if (axis_upload_id == 0) {
					axis->axisBlock[0] = (axis->specializationConstants.fftDim / 8 > 1) ? axis->specializationConstants.fftDim / 8 : 1;
					if (axis->axisBlock[0] > 512) axis->axisBlock[0] = 512;
					axis->axisBlock[1] = 1;
					axis->axisBlock[2] = 1;
					axis->axisBlock[3] = axis->specializationConstants.fftDim;
				}
				else {
					axis->axisBlock[1] = (axis->specializationConstants.fftDim / 8 > 1) ? axis->specializationConstants.fftDim / 8 : 1;

					axis->axisBlock[0] = (axis->specializationConstants.stageStartSize > axis->groupedBatch) ? axis->groupedBatch : axis->specializationConstants.stageStartSize;

					axis->axisBlock[2] = 1;
					axis->axisBlock[3] = axis->specializationConstants.fftDim;
				}
			}
			if (axis_id == 2) {
				axis->axisBlock[1] = (axis->specializationConstants.fftDim / 8 > 1) ? axis->specializationConstants.fftDim / 8 : 1;

				axis->axisBlock[0] = (configuration.size[1] > axis->groupedBatch) ? axis->groupedBatch : configuration.size[1];
				/*if (axis->axisBlock[0] * axis->axisBlock[1] < 64)
					if (configuration.size[1] > 64 / axis->axisBlock[1])
						axis->axisBlock[0] = 64 / axis->axisBlock[1];
					else
						axis->axisBlock[0] = configuration.size[0];*/
				axis->axisBlock[2] = 1;
				axis->axisBlock[3] = axis->specializationConstants.fftDim;
			}
			
			VkSpecializationMapEntry specializationMapEntries[30] = { {} };
			for (uint32_t i = 0; i < 30; i++) {
				specializationMapEntries[i].constantID = i + 1;
				specializationMapEntries[i].size = sizeof(uint32_t);
				specializationMapEntries[i].offset = i * sizeof(uint32_t);
			}
			VkSpecializationInfo specializationInfo = {};
			specializationInfo.dataSize = 30 * sizeof(uint32_t);
			specializationInfo.mapEntryCount = 30;
			specializationInfo.pMapEntries = specializationMapEntries;
			axis->specializationConstants.localSize[0] = axis->axisBlock[0];
			axis->specializationConstants.localSize[1] = axis->axisBlock[1];
			axis->specializationConstants.localSize[2] = axis->axisBlock[2];
			axis->specializationConstants.fftDim = axis->axisBlock[3];
			specializationInfo.pData = &axis->specializationConstants;
			VkPipelineShaderStageCreateInfo pipelineShaderStageCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
			VkComputePipelineCreateInfo computePipelineCreateInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };


			pipelineShaderStageCreateInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;


			if (axis_id == 1) {

				if ((configuration.FFTdim == 2) && (configuration.performConvolution) && (axis_upload_id == 0)) {
					if (axis_upload_id == 0) {
						switch (configuration.matrixConvolution) {
						case 1:
							VkFFTInitShader(9, &pipelineShaderStageCreateInfo.module);
							break;
						case 2:
							if (configuration.symmetricKernel)
								VkFFTInitShader(12, &pipelineShaderStageCreateInfo.module);
							else
								VkFFTInitShader(15, &pipelineShaderStageCreateInfo.module);
							break;
						case 3:
							if (configuration.symmetricKernel)
								VkFFTInitShader(18, &pipelineShaderStageCreateInfo.module);
							else
								VkFFTInitShader(21, &pipelineShaderStageCreateInfo.module);
							break;
						}
					}
					else {
						switch (configuration.matrixConvolution) {
						case 1:
							VkFFTInitShader(10, &pipelineShaderStageCreateInfo.module);
							break;
						case 2:
							if (configuration.symmetricKernel)
								VkFFTInitShader(13, &pipelineShaderStageCreateInfo.module);
							else
								VkFFTInitShader(16, &pipelineShaderStageCreateInfo.module);
							break;
						case 3:
							if (configuration.symmetricKernel)
								VkFFTInitShader(19, &pipelineShaderStageCreateInfo.module);
							else
								VkFFTInitShader(22, &pipelineShaderStageCreateInfo.module);
							break;
						}
					}

				}
				else {
					/*if (axis_upload_id == 0)
						VkFFTInitShader(0, &pipelineShaderStageCreateInfo.module);
					else
						VkFFTInitShader(2, &pipelineShaderStageCreateInfo.module);*/
					switch (configuration.registerBoost) {
					case 1:
					{
						if (axis_upload_id == 0)
							VkFFTInitShader(0, &pipelineShaderStageCreateInfo.module);
						else
							VkFFTInitShader(2, &pipelineShaderStageCreateInfo.module);
						break;
					}
					case 2:
					{
						switch (axis->specializationConstants.fftDim) {
						case 8192:
							VkFFTInitShader(25, &pipelineShaderStageCreateInfo.module);
							break;
						default:
							if (axis_upload_id == 0)
								VkFFTInitShader(0, &pipelineShaderStageCreateInfo.module);
							else
								VkFFTInitShader(2, &pipelineShaderStageCreateInfo.module);
							break;
						}
						break;
					}
					case 4:
					{
						switch (axis->specializationConstants.fftDim){
						case 8192:
							VkFFTInitShader(25, &pipelineShaderStageCreateInfo.module);
							break;
						case 16384:
							VkFFTInitShader(35, &pipelineShaderStageCreateInfo.module);
							break;
						default:
							if (axis_upload_id == 0)
								VkFFTInitShader(0, &pipelineShaderStageCreateInfo.module);
							else
								VkFFTInitShader(2, &pipelineShaderStageCreateInfo.module);
							break;
						}
						break;
					}
					}
				}

			}

			if (axis_id == 2) {
				if ((configuration.FFTdim == 3) && (configuration.performConvolution) && (axis_upload_id == 0)) {
						switch (configuration.matrixConvolution) {
						case 1:
							VkFFTInitShader(8, &pipelineShaderStageCreateInfo.module);
							break;
						case 2:
							if (configuration.symmetricKernel)
								VkFFTInitShader(11, &pipelineShaderStageCreateInfo.module);
							else
								VkFFTInitShader(14, &pipelineShaderStageCreateInfo.module);
							break;
						case 3:
							if (configuration.symmetricKernel)
								VkFFTInitShader(17, &pipelineShaderStageCreateInfo.module);
							else
								VkFFTInitShader(20, &pipelineShaderStageCreateInfo.module);
							break;
						}
					
				}
				else {
					VkFFTInitShader(7, &pipelineShaderStageCreateInfo.module);
				}
			}

			pipelineShaderStageCreateInfo.pName = "main";
			pipelineShaderStageCreateInfo.pSpecializationInfo = &specializationInfo;
			computePipelineCreateInfo.stage = pipelineShaderStageCreateInfo;
			computePipelineCreateInfo.layout = axis->pipelineLayout;



			vkCreateComputePipelines(configuration.device[0], VK_NULL_HANDLE, 1, &computePipelineCreateInfo, NULL, &axis->pipeline);
			vkDestroyShaderModule(configuration.device[0], pipelineShaderStageCreateInfo.module, NULL);
		}


	}
	void VkFFTPlanTranspose(VkFFTPlan* FFTPlan, uint32_t axis_id, bool inverse) {
		if (axis_id == 0) {
			if (configuration.performR2C) {
				FFTPlan->transpose[0].specializationConstants.ratio = (configuration.size[0] / configuration.size[1] / 2 >= 1) ? configuration.size[0] / configuration.size[1] / 2 : 2 * configuration.size[1] / configuration.size[0];
				FFTPlan->transpose[0].specializationConstants.ratioDirection = (configuration.size[0] / configuration.size[1] / 2 >= 1) ? true : false;
			}
			else {
				FFTPlan->transpose[0].specializationConstants.ratio = (configuration.size[0] / configuration.size[1] >= 1) ? configuration.size[0] / configuration.size[1] : configuration.size[1] / configuration.size[0];
				FFTPlan->transpose[0].specializationConstants.ratioDirection = (configuration.size[0] / configuration.size[1] >= 1) ? true : false;

			}
		}
		if (axis_id == 1) {
			FFTPlan->transpose[1].specializationConstants.ratio = (configuration.size[1] / configuration.size[2] >= 1) ? configuration.size[1] / configuration.size[2] : configuration.size[2] / configuration.size[1];
			FFTPlan->transpose[1].specializationConstants.ratioDirection = (configuration.size[1] / configuration.size[2] >= 1) ? true : false;
		}

		if (axis_id == 0) {
			if (configuration.performR2C) {
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[0] = 1;
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[1] = (FFTPlan->transpose[0].specializationConstants.ratioDirection) ? configuration.size[0] / 2 : configuration.size[1];
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[2] = (configuration.size[0] / 2 + 1) * configuration.size[1];
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[3] = (configuration.size[0] / 2 + 1) * configuration.size[1] * configuration.size[2];
			}
			else {
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[0] = 1;
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[1] = (FFTPlan->transpose[0].specializationConstants.ratioDirection) ? configuration.size[0] : configuration.size[1];
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[2] = configuration.size[0] * configuration.size[1];
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[3] = configuration.size[0] * configuration.size[1] * configuration.size[2];
			}
		}
		if (axis_id == 1) {
			if (configuration.performR2C) {
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[0] = 1;
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[1] = (FFTPlan->transpose[1].specializationConstants.ratioDirection) ? (configuration.size[0] / 2 + 1) * configuration.size[1] : (configuration.size[0] / 2 + 1) * configuration.size[2];
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[2] = (FFTPlan->transpose[1].specializationConstants.ratioDirection) ? configuration.size[1] : configuration.size[2];
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[3] = (configuration.size[0] / 2 + 1) * configuration.size[1] * configuration.size[2];
			}
			else {
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[0] = 1;
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[1] = (FFTPlan->transpose[1].specializationConstants.ratioDirection) ? configuration.size[0] * configuration.size[1] : configuration.size[0] * configuration.size[2];
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[2] = (FFTPlan->transpose[1].specializationConstants.ratioDirection) ? configuration.size[1] : configuration.size[2];
				FFTPlan->transpose[axis_id].specializationConstants.inputStride[3] = configuration.size[0] * configuration.size[1] * configuration.size[2];
			}
		}
		FFTPlan->transpose[axis_id].specializationConstants.inputStride[4] = FFTPlan->transpose[axis_id].specializationConstants.inputStride[3] * configuration.coordinateFeatures;
		VkDescriptorPoolSize descriptorPoolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
		descriptorPoolSize.descriptorCount = 2;
		//collection->descriptorNum = 3;

		VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		descriptorPoolCreateInfo.poolSizeCount = 1;
		descriptorPoolCreateInfo.pPoolSizes = &descriptorPoolSize;
		descriptorPoolCreateInfo.maxSets = 1;
		vkCreateDescriptorPool(configuration.device[0], &descriptorPoolCreateInfo, NULL, &FFTPlan->transpose[axis_id].descriptorPool);

		const VkDescriptorType descriptorType[2] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
		VkDescriptorSetLayoutBinding* descriptorSetLayoutBindings;
		descriptorSetLayoutBindings = (VkDescriptorSetLayoutBinding*)malloc(descriptorPoolSize.descriptorCount * sizeof(VkDescriptorSetLayoutBinding));
		for (uint32_t i = 0; i < descriptorPoolSize.descriptorCount; ++i) {
			descriptorSetLayoutBindings[i].binding = i;
			descriptorSetLayoutBindings[i].descriptorType = descriptorType[i];
			descriptorSetLayoutBindings[i].descriptorCount = 1;
			descriptorSetLayoutBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		}

		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		descriptorSetLayoutCreateInfo.bindingCount = descriptorPoolSize.descriptorCount;
		descriptorSetLayoutCreateInfo.pBindings = descriptorSetLayoutBindings;

		vkCreateDescriptorSetLayout(configuration.device[0], &descriptorSetLayoutCreateInfo, NULL, &FFTPlan->transpose[axis_id].descriptorSetLayout);
		free(descriptorSetLayoutBindings);
		VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		descriptorSetAllocateInfo.descriptorPool = FFTPlan->transpose[axis_id].descriptorPool;
		descriptorSetAllocateInfo.descriptorSetCount = 1;
		descriptorSetAllocateInfo.pSetLayouts = &FFTPlan->transpose[axis_id].descriptorSetLayout;
		vkAllocateDescriptorSets(configuration.device[0], &descriptorSetAllocateInfo, &FFTPlan->transpose[axis_id].descriptorSet);
		for (uint32_t i = 0; i < descriptorPoolSize.descriptorCount; ++i) {


			VkDescriptorBufferInfo descriptorBufferInfo = {};
			if (i == 0) {
				if ((configuration.numberKernels > 1) && (inverse)) {
					descriptorBufferInfo.buffer = configuration.outputBuffer[0];
					descriptorBufferInfo.range = configuration.outputBufferSize[0];
				}
				else {
					descriptorBufferInfo.buffer = configuration.buffer[0];
					descriptorBufferInfo.range = configuration.bufferSize[0];
				}
				descriptorBufferInfo.offset = 0;
			}
			if (i == 1) {
				if ((configuration.numberKernels > 1) && (inverse)) {
					descriptorBufferInfo.buffer = configuration.outputBuffer[0];
					descriptorBufferInfo.range = configuration.outputBufferSize[0];
				}
				else {
					descriptorBufferInfo.buffer = configuration.buffer[0];
					descriptorBufferInfo.range = configuration.bufferSize[0];
				}
				descriptorBufferInfo.offset = 0;
			}

			VkWriteDescriptorSet writeDescriptorSet = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			writeDescriptorSet.dstSet = FFTPlan->transpose[axis_id].descriptorSet;
			writeDescriptorSet.dstBinding = i;
			writeDescriptorSet.dstArrayElement = 0;
			writeDescriptorSet.descriptorType = descriptorType[i];
			writeDescriptorSet.descriptorCount = 1;
			writeDescriptorSet.pBufferInfo = &descriptorBufferInfo;
			vkUpdateDescriptorSets(configuration.device[0], 1, &writeDescriptorSet, 0, NULL);
		}



		VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		pipelineLayoutCreateInfo.setLayoutCount = 1;
		pipelineLayoutCreateInfo.pSetLayouts = &FFTPlan->transpose[axis_id].descriptorSetLayout;

		VkPushConstantRange pushConstantRange = { VK_SHADER_STAGE_COMPUTE_BIT };
		pushConstantRange.offset = 0;
		pushConstantRange.size = sizeof(VkFFTPushConstantsLayout);
		// Push constant ranges are part of the pipeline layout
		pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
		pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;

		vkCreatePipelineLayout(configuration.device[0], &pipelineLayoutCreateInfo, NULL, &FFTPlan->transpose[axis_id].pipelineLayout);
		VkPipelineShaderStageCreateInfo pipelineShaderStageCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };

		VkComputePipelineCreateInfo computePipelineCreateInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };

		uint32_t max_dim = 1;
		if (FFTPlan->axes[axis_id][0].axisBlock[1] * configuration.size[axis_id] < pow(2, floor(log2(sqrt(1024 * FFTPlan->transpose[axis_id].specializationConstants.ratio)))))
			max_dim = FFTPlan->axes[axis_id][0].axisBlock[1] * configuration.size[axis_id];
		else
			max_dim = pow(2, floor(log2(sqrt(1024 * FFTPlan->transpose[axis_id].specializationConstants.ratio))));
		FFTPlan->transpose[axis_id].transposeBlock[0] = max_dim;
		FFTPlan->transpose[axis_id].transposeBlock[1] = max_dim / FFTPlan->transpose[axis_id].specializationConstants.ratio;
		FFTPlan->transpose[axis_id].transposeBlock[2] = 1;

		VkSpecializationMapEntry specializationMapEntries[12] = { {} };
		for (uint32_t i = 0; i < 12; i++) {
			specializationMapEntries[i].constantID = i + 1;
			specializationMapEntries[i].size = sizeof(uint32_t);
			specializationMapEntries[i].offset = i * sizeof(uint32_t);
		}
		VkSpecializationInfo specializationInfo = {};
		specializationInfo.dataSize = 12 * sizeof(uint32_t);
		specializationInfo.mapEntryCount = 12;
		specializationInfo.pMapEntries = specializationMapEntries;
		FFTPlan->transpose[axis_id].specializationConstants.localSize[0] = FFTPlan->transpose[axis_id].transposeBlock[0];
		FFTPlan->transpose[axis_id].specializationConstants.localSize[1] = FFTPlan->transpose[axis_id].transposeBlock[1];
		FFTPlan->transpose[axis_id].specializationConstants.localSize[2] = FFTPlan->transpose[axis_id].transposeBlock[2];

		specializationInfo.pData = &FFTPlan->transpose[axis_id].specializationConstants;

		pipelineShaderStageCreateInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;

		uint32_t filelength;
		//printf("vkFFT_transpose_inplace\n");
		char filename[256];
		sprintf(filename, "%s%s", configuration.shaderPath, "vkFFT_transpose_inplace.spv");

		uint32_t* code = VkFFTReadShader(filelength, filename);
		VkShaderModuleCreateInfo createInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		createInfo.pCode = code;
		createInfo.codeSize = filelength;
		vkCreateShaderModule(configuration.device[0], &createInfo, NULL, &pipelineShaderStageCreateInfo.module);
		free(code);

		pipelineShaderStageCreateInfo.pSpecializationInfo = &specializationInfo;
		pipelineShaderStageCreateInfo.pName = "main";
		computePipelineCreateInfo.stage = pipelineShaderStageCreateInfo;
		computePipelineCreateInfo.layout = FFTPlan->transpose[axis_id].pipelineLayout;


		vkCreateComputePipelines(configuration.device[0], VK_NULL_HANDLE, 1, &computePipelineCreateInfo, NULL, &FFTPlan->transpose[axis_id].pipeline);
		vkDestroyShaderModule(configuration.device[0], pipelineShaderStageCreateInfo.module, NULL);

	}
	void deleteAxis(VkFFTAxis* axis) {
		vkDestroyDescriptorPool(configuration.device[0], axis->descriptorPool, NULL);
		vkDestroyDescriptorSetLayout(configuration.device[0], axis->descriptorSetLayout, NULL);
		vkDestroyPipelineLayout(configuration.device[0], axis->pipelineLayout, NULL);
		vkDestroyPipeline(configuration.device[0], axis->pipeline, NULL);


	}
	void deleteTranspose(VkFFTTranspose* transpose) {
		vkDestroyDescriptorPool(configuration.device[0], transpose->descriptorPool, NULL);
		vkDestroyDescriptorSetLayout(configuration.device[0], transpose->descriptorSetLayout, NULL);
		vkDestroyPipelineLayout(configuration.device[0], transpose->pipelineLayout, NULL);
		vkDestroyPipeline(configuration.device[0], transpose->pipeline, NULL);


	}
	void initializeVulkanFFT(VkFFTConfiguration inputLaunchConfiguration) {
		configuration = inputLaunchConfiguration;
		if (configuration.matrixConvolution > 1) configuration.coordinateFeatures = configuration.matrixConvolution;

		if (configuration.performConvolution) {
			
			configuration.inverse = false;
			for (uint32_t i = 0; i < configuration.FFTdim; i++) {
				for (uint32_t j =0; j<8; j++)
					VkFFTPlanAxis(&localFFTPlan_inverse_convolution, i, j, true);
			}
			
		}
		for (uint32_t i = 0; i < configuration.FFTdim; i++) {
			for (uint32_t j = 0; j < 8; j++)
				VkFFTPlanAxis(&localFFTPlan, i, j, configuration.inverse);
		}

	}
	void VkFFTAppend(VkCommandBuffer commandBuffer) {
		VkMemoryBarrier memory_barrier = {
				VK_STRUCTURE_TYPE_MEMORY_BARRIER,
				nullptr,
				VK_ACCESS_SHADER_WRITE_BIT,
				VK_ACCESS_SHADER_READ_BIT,
		};
		if (!configuration.inverse) {
			//FFT axis 0
			for (uint32_t j = 0; j < configuration.numberBatches; j++) {
				for (int l = localFFTPlan.numAxisUploads[0]-1; l >=0; l--) {
					VkFFTAxis* axis = &localFFTPlan.axes[0][l];
					axis->pushConstants.batch = j;
					uint32_t maxCoordinate = ((configuration.matrixConvolution) > 1 && (configuration.performConvolution) && (configuration.FFTdim == 1)) ? 1 : configuration.coordinateFeatures;
					for (uint32_t i = 0; i < maxCoordinate; i++) {
						axis->pushConstants.coordinate = i;
						vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
						vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
						vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
						if (l == 0) {
							if (configuration.performZeropadding[1]) {
								if (configuration.performZeropadding[2]) {

									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0 / 2.0), ceil(configuration.size[2] / 2.0));
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0), ceil(configuration.size[2] / 2.0));
								}
								else {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0 / 2.0), configuration.size[2]);
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0) , configuration.size[2]);
								}
							}
							else {
								if (configuration.performZeropadding[2]) {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0) , ceil(configuration.size[2] / 2.0));
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, configuration.size[1] , ceil(configuration.size[2] / 2.0));
								}
								else {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0) , configuration.size[2]);
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, configuration.size[1], configuration.size[2]);
								}
							}
						}
						else {
							if (configuration.performZeropadding[1]) {
								if (configuration.performZeropadding[2]) {

									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0 / 2.0), ceil(configuration.size[2] / 2.0));
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0), ceil(configuration.size[2] / 2.0));
								}
								else {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0 / 2.0), configuration.size[2] );
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0), configuration.size[2]);
								}
							}
							else {
								if (configuration.performZeropadding[2]) {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0) , ceil(configuration.size[2] / 2.0));
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], configuration.size[1] , ceil(configuration.size[2] / 2.0));
								}
								else {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0) , configuration.size[2] );
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], configuration.size[1] , configuration.size[2]);
								}
							}
						}
					}
					vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
				}
			}
			
			if (configuration.FFTdim > 1) {
				//transpose 0-1, if needed
				/*if (configuration.performTranspose[0]) {
					for (uint32_t j = 0; j < configuration.numberBatches; j++) {
						localFFTPlan.transpose[0].pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							localFFTPlan.transpose[0].pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, localFFTPlan.transpose[0].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.transpose[0].pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.transpose[0].pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.transpose[0].pipelineLayout, 0, 1, &localFFTPlan.transpose[0].descriptorSet, 0, NULL);
							if (configuration.performR2C == true) {
								if (localFFTPlan.transpose[0].specializationConstants.ratioDirection)
									vkCmdDispatch(commandBuffer, configuration.size[0] / 2 / localFFTPlan.transpose[0].transposeBlock[0], configuration.size[1] / localFFTPlan.transpose[0].transposeBlock[1], configuration.size[2] / localFFTPlan.transpose[0].transposeBlock[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.transpose[0].transposeBlock[0], configuration.size[0] / 2 / localFFTPlan.transpose[0].transposeBlock[1], configuration.size[2] / localFFTPlan.transpose[0].transposeBlock[2]);

							}
							else {
								if (localFFTPlan.transpose[0].specializationConstants.ratioDirection)
									vkCmdDispatch(commandBuffer, configuration.size[0] / localFFTPlan.transpose[0].transposeBlock[0], configuration.size[1] / localFFTPlan.transpose[0].transposeBlock[1], configuration.size[2] / localFFTPlan.transpose[0].transposeBlock[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.transpose[0].transposeBlock[0], configuration.size[0] / localFFTPlan.transpose[0].transposeBlock[1], configuration.size[2] / localFFTPlan.transpose[0].transposeBlock[2]);

							}
						}
					}
					vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

				}*/

				//FFT axis 1
				if ((configuration.FFTdim == 2) && (configuration.performConvolution)) {
					/*if (configuration.performTranspose[0]) {
						uint32_t maxCoordinate = (configuration.matrixConvolution > 1 ) ? 1 : configuration.coordinateFeatures;
						for (uint32_t i = 0; i < maxCoordinate; i++) {
							localFFTPlan.axes[1].pushConstants.coordinate = i;
							localFFTPlan.axes[1].pushConstants.batch = configuration.numberKernels;
							vkCmdPushConstants(commandBuffer, localFFTPlan.axes[1].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.axes[1].pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[1].pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[1].pipelineLayout, 0, 1, &localFFTPlan.axes[1].descriptorSet, 0, NULL);
							if (configuration.performZeropadding[2]) {
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, 1, configuration.size[0] / 2 / localFFTPlan.axes[1].axisBlock[1] + 1, ceil(configuration.size[2] / 2.0 / localFFTPlan.axes[1].axisBlock[2]));
								else
									vkCmdDispatch(commandBuffer, 1, configuration.size[0] / localFFTPlan.axes[1].axisBlock[1], ceil(configuration.size[2] / 2.0 / localFFTPlan.axes[1].axisBlock[2]));
							}
							else {
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, 1, configuration.size[0] / 2 / localFFTPlan.axes[1].axisBlock[1] + 1, configuration.size[2] / localFFTPlan.axes[1].axisBlock[2]);
								else
									vkCmdDispatch(commandBuffer, 1, configuration.size[0] / localFFTPlan.axes[1].axisBlock[1], configuration.size[2] / localFFTPlan.axes[1].axisBlock[2]);

							}
						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

					}
					else {*/
					if (configuration.performR2C == true) {
						for (int l = localFFTPlan.numSupportAxisUploads[0]-1; l >=0; l--) {
							VkFFTAxis* axis = &localFFTPlan.supportAxes[0][l];
							uint32_t maxCoordinate = ((configuration.matrixConvolution > 1)&&(l == 0)) ? 1 : configuration.coordinateFeatures;
							for (uint32_t i = 0; i < maxCoordinate; i++) {
								axis->pushConstants.coordinate = i;
								
								axis->pushConstants.batch = ((l == 0)&& (configuration.matrixConvolution == 1)) ? configuration.numberKernels : 0;

								vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
								vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
								vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
								if (l == 0) {
									if (configuration.performZeropadding[2]) {
										vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
									}
									else {
										vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);
									}
								}
								else{
									if (configuration.performZeropadding[2]) {
										vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim / axis->axisBlock[0], 1, ceil(configuration.size[2] / 2.0));
									}
									else {
										vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim / axis->axisBlock[0], 1, configuration.size[2]);
									}
								}
							}
							if (l >0)
								vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

						}
						
					}
					
					for (int l = localFFTPlan.numAxisUploads[1]-1; l >=0; l--) {
						VkFFTAxis* axis = &localFFTPlan.axes[1][l];
						uint32_t maxCoordinate = ((configuration.matrixConvolution > 1) && (l == 0)) ? 1 : configuration.coordinateFeatures;
						for (uint32_t i = 0; i < maxCoordinate; i++) {

							axis->pushConstants.coordinate = i;
							axis->pushConstants.batch = ((l == 0) && (configuration.matrixConvolution == 1)) ? configuration.numberKernels : 0;
							vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
							if (configuration.performZeropadding[2]) {
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0]* configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
								else
									vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0]* configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
							}
							else {
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0]* configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2] );
								else
									vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0]* configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2] );

							}
						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

					}
					
					//}
				}
				else {
					/*if (configuration.performTranspose[0]) {
						for (uint32_t j = 0; j < configuration.numberBatches; j++) {
							localFFTPlan.axes[1].pushConstants.batch = j;
							for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
								localFFTPlan.axes[1].pushConstants.coordinate = i;
								vkCmdPushConstants(commandBuffer, localFFTPlan.axes[1].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.axes[1].pushConstants);
								vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[1].pipeline);
								vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[1].pipelineLayout, 0, 1, &localFFTPlan.axes[1].descriptorSet, 0, NULL);
								if (configuration.performZeropadding[2]) {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, 1, configuration.size[0] / 2 / localFFTPlan.axes[1].axisBlock[1] + 1, ceil(configuration.size[2] / 2.0 / localFFTPlan.axes[1].axisBlock[2]));
									else
										vkCmdDispatch(commandBuffer, 1, configuration.size[0] / localFFTPlan.axes[1].axisBlock[1], ceil(configuration.size[2] / 2.0 / localFFTPlan.axes[1].axisBlock[2]));
								}
								else {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, 1, configuration.size[0] / 2 / localFFTPlan.axes[1].axisBlock[1] + 1, configuration.size[2] / localFFTPlan.axes[1].axisBlock[2]);
									else
										vkCmdDispatch(commandBuffer, 1, configuration.size[0] / localFFTPlan.axes[1].axisBlock[1], configuration.size[2] / localFFTPlan.axes[1].axisBlock[2]);

								}

							}
						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

					}
					else {
					*/
					if (configuration.performR2C == true) {
						for (uint32_t j = 0; j < configuration.numberBatches; j++) {
							for (int l = localFFTPlan.numSupportAxisUploads[0]-1; l >=0; l--) {
								VkFFTAxis* axis = &localFFTPlan.supportAxes[0][l];
								axis->pushConstants.batch = j;
								for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
									axis->pushConstants.coordinate = i;
									vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
									vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
									vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
									if (l == 0) {
										if (configuration.performZeropadding[2]) {
											vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil (configuration.size[2] / 2.0));
										}
										else {
											vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);
										}
									}
									else {
										if (configuration.performZeropadding[2]) {
											vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim / axis->axisBlock[0], 1, ceil(configuration.size[2] / 2.0));
										}
										else {
											vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim / axis->axisBlock[0], 1, configuration.size[2]);
										}
									}
								}
								if (l >=0)
									vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

							}
						}
					}
					for (uint32_t j = 0; j < configuration.numberBatches; j++) {
						for (int l = localFFTPlan.numAxisUploads[1]-1; l >=0; l--) {
							VkFFTAxis* axis = &localFFTPlan.axes[1][l];
							axis->pushConstants.batch = j;
							for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
								axis->pushConstants.coordinate = i;
								vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
								vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
								vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
								if (configuration.performZeropadding[2]) {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
								}
								else {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);

								}
							}
							vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

						}
					}
					
					//}
				}
			}
			//FFT axis 2
			if (configuration.FFTdim > 2) {
				//transpose 1-2, after 0-1
				/*if (configuration.performTranspose[1]) {
					for (uint32_t j = 0; j < configuration.numberBatches; j++) {
						localFFTPlan.transpose[1].pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							localFFTPlan.transpose[1].pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, localFFTPlan.transpose[1].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.transpose[1].pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.transpose[1].pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.transpose[1].pipelineLayout, 0, 1, &localFFTPlan.transpose[1].descriptorSet, 0, NULL);
							if (configuration.performR2C == true) {
								if (localFFTPlan.transpose[1].specializationConstants.ratioDirection)
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.transpose[1].transposeBlock[0], configuration.size[2] / localFFTPlan.transpose[1].transposeBlock[1], (configuration.size[0] / 2 + 1) / localFFTPlan.transpose[1].transposeBlock[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[2] / localFFTPlan.transpose[1].transposeBlock[0], configuration.size[1] / localFFTPlan.transpose[1].transposeBlock[1], (configuration.size[0] / 2 + 1) / localFFTPlan.transpose[1].transposeBlock[2]);

							}
							else {
								if (localFFTPlan.transpose[1].specializationConstants.ratioDirection)
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.transpose[1].transposeBlock[0], configuration.size[2] / localFFTPlan.transpose[1].transposeBlock[1], configuration.size[0] / localFFTPlan.transpose[1].transposeBlock[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[2] / localFFTPlan.transpose[1].transposeBlock[0], configuration.size[1] / localFFTPlan.transpose[1].transposeBlock[1], configuration.size[0] / localFFTPlan.transpose[1].transposeBlock[2]);

							}
						}
					}
					vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

				}*/

				if ((configuration.FFTdim == 3) && (configuration.performConvolution)) {
					//transposed 1-2, transposed 0-1
					/*if (configuration.performTranspose[1]) {
						uint32_t maxCoordinate = (configuration.matrixConvolution > 1) ? 1 : configuration.coordinateFeatures;
						for (uint32_t i = 0; i < maxCoordinate; i++) {
							localFFTPlan.axes[2].pushConstants.coordinate = i;
							localFFTPlan.axes[2].pushConstants.batch = configuration.numberKernels;
							vkCmdPushConstants(commandBuffer, localFFTPlan.axes[2].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.axes[2].pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[2].pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[2].pipelineLayout, 0, 1, &localFFTPlan.axes[2].descriptorSet, 0, NULL);
							if (configuration.performR2C == true)
								vkCmdDispatch(commandBuffer, 1, configuration.size[1] / localFFTPlan.axes[2].axisBlock[2], configuration.size[0] / 2 + 1);
							else
								vkCmdDispatch(commandBuffer, 1, configuration.size[1] / localFFTPlan.axes[2].axisBlock[2], configuration.size[0]);
						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

					}
					else {
					if (configuration.performTranspose[0]) {
						//transposed 0-1, didn't transpose 1-2
						uint32_t maxCoordinate = (configuration.matrixConvolution > 1) ? 1 : configuration.coordinateFeatures;
						for (uint32_t i = 0; i < maxCoordinate; i++) {
							localFFTPlan.axes[2].pushConstants.coordinate = i;
							localFFTPlan.axes[2].pushConstants.batch = configuration.numberKernels;
							vkCmdPushConstants(commandBuffer, localFFTPlan.axes[2].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.axes[2].pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[2].pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[2].pipelineLayout, 0, 1, &localFFTPlan.axes[2].descriptorSet, 0, NULL);
							if (configuration.performR2C == true)
								vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.axes[2].axisBlock[0], 1, configuration.size[0] / 2 + 1);
							else
								vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.axes[2].axisBlock[0], 1, configuration.size[0]);
						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
					}
					else {*/
						//didn't transpose 0-1, didn't transpose 1-2
					if (configuration.performR2C == true) {

						for (int l = localFFTPlan.numSupportAxisUploads[1]-1; l >= 0; l--) {
							VkFFTAxis* axis = &localFFTPlan.supportAxes[1][l];
							uint32_t maxCoordinate = ((configuration.matrixConvolution > 1) && (l == 0)) ? 1 : configuration.coordinateFeatures;
							for (uint32_t i = 0; i < maxCoordinate; i++) {
								axis->pushConstants.coordinate = i;
								
								axis->pushConstants.batch = ((l == 0) && (configuration.matrixConvolution == 1)) ? configuration.numberKernels : 0;

								vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
								vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
								vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
								vkCmdDispatch(commandBuffer, configuration.size[1] / axis->axisBlock[0]* configuration.size[2] / axis->specializationConstants.fftDim, 1, 1);

							}
							if (l >=0)
								vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

						}
					}
	
					for (int l= localFFTPlan.numAxisUploads[2]-1; l >=0; l--) {

						VkFFTAxis* axis = &localFFTPlan.axes[2][l];
						uint32_t maxCoordinate = ((configuration.matrixConvolution > 1) && (l == 0)) ? 1 : configuration.coordinateFeatures;
						for (uint32_t i = 0; i < maxCoordinate; i++) {
							axis->pushConstants.coordinate = i;
							axis->pushConstants.batch = ((l == 0) && (configuration.matrixConvolution == 1)) ? configuration.numberKernels : 0;

							vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
							if (configuration.performR2C == true)
								vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[2] / axis->specializationConstants.fftDim, 1, configuration.size[1]);
							else
								vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[2] / axis->specializationConstants.fftDim, 1, configuration.size[1]);
						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

					}
					//}
					//}
				}
				else {
					//transposed 1-2, transposed 0-1
					/*if (configuration.performTranspose[1]) {
						for (uint32_t j = 0; j < configuration.numberBatches; j++) {
							localFFTPlan.axes[2].pushConstants.batch = j;
							for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
								localFFTPlan.axes[2].pushConstants.coordinate = i;
								vkCmdPushConstants(commandBuffer, localFFTPlan.axes[2].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.axes[2].pushConstants);
								vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[2].pipeline);
								vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[2].pipelineLayout, 0, 1, &localFFTPlan.axes[2].descriptorSet, 0, NULL);
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, 1, configuration.size[1] / localFFTPlan.axes[2].axisBlock[2], configuration.size[0] / 2 + 1);
								else
									vkCmdDispatch(commandBuffer, 1, configuration.size[1] / localFFTPlan.axes[2].axisBlock[2], configuration.size[0]);
							}
						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

					}
					else {
						if (configuration.performTranspose[0]) {
							//transposed 0-1, didn't transpose 1-2
							for (uint32_t j = 0; j < configuration.numberBatches; j++) {
								localFFTPlan.axes[2].pushConstants.batch = j;
								for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
									localFFTPlan.axes[2].pushConstants.coordinate = i;
									vkCmdPushConstants(commandBuffer, localFFTPlan.axes[2].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.axes[2].pushConstants);
									vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[2].pipeline);
									vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[2].pipelineLayout, 0, 1, &localFFTPlan.axes[2].descriptorSet, 0, NULL);
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.axes[2].axisBlock[0], 1, configuration.size[0] / 2 + 1);
									else
										vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.axes[2].axisBlock[0], 1, configuration.size[0]);
								}
							}
							vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

						}
						else {*/
					//didn't transpose 0-1, didn't transpose 1-2
					if (configuration.performR2C == true) {
						for (uint32_t j = 0; j < configuration.numberBatches; j++) {
							for (int l = localFFTPlan.numSupportAxisUploads[1]-1; l >= 0; l--) {
								VkFFTAxis* axis = &localFFTPlan.supportAxes[1][l];
								axis->pushConstants.batch = j;
								for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
									axis->pushConstants.coordinate = i;
									
									vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
									vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
									vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
									vkCmdDispatch(commandBuffer, configuration.size[1] / axis->axisBlock[0] * configuration.size[2] / axis->specializationConstants.fftDim, 1, 1);

								}
								if (l >= 0)
									vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

							}
						}
					}
					for (uint32_t j = 0; j < configuration.numberBatches; j++) {
						for (int l = localFFTPlan.numAxisUploads[2]-1; l >=0; l--) {
							VkFFTAxis* axis = &localFFTPlan.axes[2][l];
							axis->pushConstants.batch = j;
							for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
								axis->pushConstants.coordinate = i;
								vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
								vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
								vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[2] / axis->specializationConstants.fftDim, 1, configuration.size[1]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[2] / axis->specializationConstants.fftDim, 1, configuration.size[1]);
							}
							vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

						}
					}
					
						//}
					//}
				}

			}
		}
		if (configuration.performConvolution) {
			if (configuration.FFTdim > 2) {

				//transpose 1-2, after 0-1
				/*if (configuration.performTranspose[1]) {
					for (uint32_t j = 0; j < configuration.numberKernels; j++) {
						localFFTPlan_inverse_convolution.transpose[1].pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							localFFTPlan_inverse_convolution.transpose[1].pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, localFFTPlan_inverse_convolution.transpose[1].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan_inverse_convolution.transpose[1].pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan_inverse_convolution.transpose[1].pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan_inverse_convolution.transpose[1].pipelineLayout, 0, 1, &localFFTPlan_inverse_convolution.transpose[1].descriptorSet, 0, NULL);
							if (configuration.performR2C == true) {
								if (localFFTPlan_inverse_convolution.transpose[1].specializationConstants.ratioDirection)
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan_inverse_convolution.transpose[1].transposeBlock[0], configuration.size[2] / localFFTPlan_inverse_convolution.transpose[1].transposeBlock[1], (configuration.size[0] / 2 + 1) / localFFTPlan_inverse_convolution.transpose[1].transposeBlock[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[2] / localFFTPlan_inverse_convolution.transpose[1].transposeBlock[0], configuration.size[1] / localFFTPlan_inverse_convolution.transpose[1].transposeBlock[1], (configuration.size[0] / 2 + 1) / localFFTPlan_inverse_convolution.transpose[1].transposeBlock[2]);

							}
							else {
								if (localFFTPlan_inverse_convolution.transpose[1].specializationConstants.ratioDirection)
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan_inverse_convolution.transpose[1].transposeBlock[0], configuration.size[2] / localFFTPlan_inverse_convolution.transpose[1].transposeBlock[1], configuration.size[0] / localFFTPlan_inverse_convolution.transpose[1].transposeBlock[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[2] / localFFTPlan_inverse_convolution.transpose[1].transposeBlock[0], configuration.size[1] / localFFTPlan_inverse_convolution.transpose[1].transposeBlock[1], configuration.size[0] / localFFTPlan_inverse_convolution.transpose[1].transposeBlock[2]);

							}
						}
					}
					vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

				}

				if (configuration.performTranspose[0]) {
					for (uint32_t j = 0; j < configuration.numberKernels; j++) {
						localFFTPlan_inverse_convolution.axes[1].pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							localFFTPlan_inverse_convolution.axes[1].pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, localFFTPlan_inverse_convolution.axes[1].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan_inverse_convolution.axes[1].pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan_inverse_convolution.axes[1].pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan_inverse_convolution.axes[1].pipelineLayout, 0, 1, &localFFTPlan_inverse_convolution.axes[1].descriptorSet, 0, NULL);
							if (configuration.performZeropadding[2]) {
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, 1, configuration.size[0] / 2 / localFFTPlan_inverse_convolution.axes[1].axisBlock[1] + 1, ceil(configuration.size[2] / 2.0 / localFFTPlan_inverse_convolution.axes[1].axisBlock[2]));
								else
									vkCmdDispatch(commandBuffer, 1, configuration.size[0] / localFFTPlan_inverse_convolution.axes[1].axisBlock[1], ceil(configuration.size[2] / 2.0 / localFFTPlan_inverse_convolution.axes[1].axisBlock[2]));
							}
							else {
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, 1, configuration.size[0] / 2 / localFFTPlan_inverse_convolution.axes[1].axisBlock[1] + 1, configuration.size[2] / localFFTPlan_inverse_convolution.axes[1].axisBlock[2]);
								else
									vkCmdDispatch(commandBuffer, 1, configuration.size[0] / localFFTPlan_inverse_convolution.axes[1].axisBlock[1], configuration.size[2] / localFFTPlan_inverse_convolution.axes[1].axisBlock[2]);

							}
						}
					}
					vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
				}
				else {*/
				//multiple upload ifft leftovers
				if (configuration.FFTdim == 3) {
					if (configuration.performR2C == true) {
						for (uint32_t j = 0; j < configuration.numberKernels; j++) {
							for (int l = 1; l< localFFTPlan_inverse_convolution.numSupportAxisUploads[1]; l++) {
								VkFFTAxis* axis = &localFFTPlan_inverse_convolution.supportAxes[1][l];
								uint32_t maxCoordinate = configuration.coordinateFeatures;
								for (uint32_t i = 0; i < maxCoordinate; i++) {
									axis->pushConstants.coordinate = i;
									axis->pushConstants.batch = j;
									vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
									vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
									vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
									vkCmdDispatch(commandBuffer, configuration.size[1] / axis->axisBlock[0] * configuration.size[2] / axis->specializationConstants.fftDim, 1, 1);

								}
								if (l > 0)
									vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

							}
						}
					}
					for (uint32_t j = 0; j < configuration.numberKernels; j++) {
						for (int l = 1; l <  localFFTPlan_inverse_convolution.numAxisUploads[2]; l++) {
							VkFFTAxis* axis = &localFFTPlan_inverse_convolution.axes[2][l];
							uint32_t maxCoordinate = configuration.coordinateFeatures;
							for (uint32_t i = 0; i < maxCoordinate; i++) {
								axis->pushConstants.coordinate = i;
								axis->pushConstants.batch = j;
								vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
								vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
								vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[2] / axis->specializationConstants.fftDim, 1, configuration.size[1]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[2] / axis->specializationConstants.fftDim, 1, configuration.size[1]);
							}
							vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

						}
					}
				}
				if (configuration.performR2C == true) {
					for (uint32_t j = 0; j < configuration.numberKernels; j++) {
						for (int l = localFFTPlan_inverse_convolution.numSupportAxisUploads[0]-1; l >=0; l--) {
							VkFFTAxis* axis = &localFFTPlan_inverse_convolution.supportAxes[0][l];
							axis->pushConstants.batch = j;
							for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {

								axis->pushConstants.coordinate = i;
								vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
								vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
								vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
								if (l == 0) {
									if (configuration.performZeropadding[2]) {
										vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
									}
									else {
										vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);
									}
								}
								else {
									if (configuration.performZeropadding[2]) {
										vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim / axis->axisBlock[0], 1, ceil(configuration.size[2] / 2.0));
									}
									else {
										vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim / axis->axisBlock[0], 1, configuration.size[2]);
									}
								}
							}
							if (l >= 0)
								vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

						}
					}
				}
				for (uint32_t j = 0; j < configuration.numberKernels; j++) {
					for (int l = localFFTPlan_inverse_convolution.numAxisUploads[1]-1; l >= 0; l--) {
						VkFFTAxis* axis = &localFFTPlan_inverse_convolution.axes[1][l];
						axis->pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							axis->pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
							if (configuration.performZeropadding[2]) {
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
								else
									vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
							}
							else {
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2] );
								else
									vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2] );

							}
						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

					}
				}
				
				//}




			}
			if (configuration.FFTdim > 1) {
				// transpose 0 - 1, if needed
				/*if (configuration.performTranspose[0]) {
					for (uint32_t j = 0; j < configuration.numberKernels; j++) {
						localFFTPlan_inverse_convolution.transpose[0].pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							localFFTPlan_inverse_convolution.transpose[0].pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, localFFTPlan_inverse_convolution.transpose[0].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan_inverse_convolution.transpose[0].pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan_inverse_convolution.transpose[0].pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan_inverse_convolution.transpose[0].pipelineLayout, 0, 1, &localFFTPlan_inverse_convolution.transpose[0].descriptorSet, 0, NULL);
							if (configuration.performR2C == true) {
								if (localFFTPlan_inverse_convolution.transpose[0].specializationConstants.ratioDirection)
									vkCmdDispatch(commandBuffer, configuration.size[0] / 2 / localFFTPlan_inverse_convolution.transpose[0].transposeBlock[0], configuration.size[1] / localFFTPlan_inverse_convolution.transpose[0].transposeBlock[1], configuration.size[2] / localFFTPlan_inverse_convolution.transpose[0].transposeBlock[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan_inverse_convolution.transpose[0].transposeBlock[0], configuration.size[0] / 2 / localFFTPlan_inverse_convolution.transpose[0].transposeBlock[1], configuration.size[2] / localFFTPlan_inverse_convolution.transpose[0].transposeBlock[2]);

							}
							else {
								if (localFFTPlan_inverse_convolution.transpose[0].specializationConstants.ratioDirection)
									vkCmdDispatch(commandBuffer, configuration.size[0] / localFFTPlan_inverse_convolution.transpose[0].transposeBlock[0], configuration.size[1] / localFFTPlan_inverse_convolution.transpose[0].transposeBlock[1], configuration.size[2] / localFFTPlan_inverse_convolution.transpose[0].transposeBlock[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan_inverse_convolution.transpose[0].transposeBlock[0], configuration.size[0] / localFFTPlan_inverse_convolution.transpose[0].transposeBlock[1], configuration.size[2] / localFFTPlan_inverse_convolution.transpose[0].transposeBlock[2]);

							}
						}
					}
					vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

				}*/
				if (configuration.FFTdim == 2) {
					if (configuration.performR2C == true) {
						for (uint32_t j = 0; j < configuration.numberKernels; j++) {
							for (int l = 1; l< localFFTPlan_inverse_convolution.numSupportAxisUploads[0]; l++) {
								VkFFTAxis* axis = &localFFTPlan_inverse_convolution.supportAxes[0][l];
								uint32_t maxCoordinate = configuration.coordinateFeatures;
								for (uint32_t i = 0; i < maxCoordinate; i++) {
									axis->pushConstants.coordinate = i;
									axis->pushConstants.batch = j;
									vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
									vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
									vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
									if (l == 0) {
										if (configuration.performZeropadding[2]) {
											vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
										}
										else {
											vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);
										}
									}
									else {
										if (configuration.performZeropadding[2]) {
											vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim / axis->axisBlock[0], 1, ceil(configuration.size[2] / 2.0));
										}
										else {
											vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim / axis->axisBlock[0], 1, configuration.size[2]);
										}
									}
								}
								if (l > 0)
									vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

							}
						}

					}
					for (uint32_t j = 0; j < configuration.numberKernels; j++) {
						for (int l = 1; l< localFFTPlan_inverse_convolution.numAxisUploads[1]; l++) {
							VkFFTAxis* axis = &localFFTPlan_inverse_convolution.axes[1][l];
							uint32_t maxCoordinate = configuration.coordinateFeatures;
							for (uint32_t i = 0; i < maxCoordinate; i++) {

								axis->pushConstants.coordinate = i;
								axis->pushConstants.batch = j;
								vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
								vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
								vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
								if (configuration.performZeropadding[2]) {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
								}
								else {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);

								}
							}
							vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

						}
					}
				}
				for (uint32_t j = 0; j < configuration.numberKernels; j++) {
					for (int l = localFFTPlan_inverse_convolution.numAxisUploads[0]-1; l >= 0; l--) {
						VkFFTAxis* axis = &localFFTPlan_inverse_convolution.axes[0][l];
						axis->pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							axis->pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
							if (l == 0) {
								if (configuration.performZeropadding[1]) {
									if (configuration.performZeropadding[2]) {

										if (configuration.performR2C == true)
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0 / 2.0), ceil(configuration.size[2] / 2.0));
										else
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0), ceil(configuration.size[2] / 2.0));
									}
									else {
										if (configuration.performR2C == true)
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0 / 2.0), configuration.size[2]);
										else
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0), configuration.size[2]);
									}
								}
								else {
									if (configuration.performZeropadding[2]) {
										if (configuration.performR2C == true)
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0), ceil(configuration.size[2] / 2.0));
										else
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, configuration.size[1], ceil(configuration.size[2] / 2.0));
									}
									else {
										if (configuration.performR2C == true)
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0), configuration.size[2]);
										else
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, configuration.size[1], configuration.size[2]);
									}
								}
							}
							else {
								if (configuration.performZeropadding[1]) {
									if (configuration.performZeropadding[2]) {

										if (configuration.performR2C == true)
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0 / 2.0), ceil(configuration.size[2] / 2.0));
										else
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0), ceil(configuration.size[2] / 2.0));
									}
									else {
										if (configuration.performR2C == true)
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0 / 2.0), configuration.size[2]);
										else
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0), configuration.size[2]);
									}
								}
								else {
									if (configuration.performZeropadding[2]) {
										if (configuration.performR2C == true)
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0), ceil(configuration.size[2] / 2.0));
										else
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], configuration.size[1], ceil(configuration.size[2] / 2.0));
									}
									else {
										if (configuration.performR2C == true)
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0), configuration.size[2]);
										else
											vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], configuration.size[1], configuration.size[2]);
									}
								}
							}

						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

					}
				}
				

			}
			if (configuration.FFTdim == 1) {
				for (uint32_t j = 0; j < configuration.numberKernels; j++) {
					for (int l = 1; l < localFFTPlan_inverse_convolution.numAxisUploads[0]; l++) {
						VkFFTAxis* axis = &localFFTPlan_inverse_convolution.axes[0][l];
						uint32_t maxCoordinate = configuration.coordinateFeatures;
						for (uint32_t i = 0; i < maxCoordinate; i++) {

							axis->pushConstants.coordinate = i;
							axis->pushConstants.batch = j;
							vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
							if (configuration.performZeropadding[2]) {
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
								else
									vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
							}
							else {
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);

							}
						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

					}
				}
			}
		}

		if (configuration.inverse) {
			//we start from axis 2 and go back to axis 0
			//FFT axis 2
			if (configuration.FFTdim > 2) {
				//transposed 1-2, transposed 0-1
				/*if (configuration.performTranspose[1]) {
					for (uint32_t j = 0; j < configuration.numberBatches; j++) {
						localFFTPlan.axes[2].pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							localFFTPlan.axes[2].pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, localFFTPlan.axes[2].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.axes[2].pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[2].pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[2].pipelineLayout, 0, 1, &localFFTPlan.axes[2].descriptorSet, 0, NULL);
							if (configuration.performR2C == true)
								vkCmdDispatch(commandBuffer, 1, configuration.size[1] / localFFTPlan.axes[2].axisBlock[2], configuration.size[0] / 2 + 1);
							else
								vkCmdDispatch(commandBuffer, 1, configuration.size[1] / localFFTPlan.axes[2].axisBlock[2], configuration.size[0]);
						}
					}
					vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

				}
				else {
					if (configuration.performTranspose[0]) {
						//transposed 0-1, didn't transpose 1-2
						for (uint32_t j = 0; j < configuration.numberBatches; j++) {
							localFFTPlan.axes[2].pushConstants.batch = j;
							for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
								localFFTPlan.axes[2].pushConstants.coordinate = i;
								vkCmdPushConstants(commandBuffer, localFFTPlan.axes[2].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.axes[2].pushConstants);
								vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[2].pipeline);
								vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[2].pipelineLayout, 0, 1, &localFFTPlan.axes[2].descriptorSet, 0, NULL);
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.axes[2].axisBlock[0], 1, configuration.size[0] / 2 + 1);
								else
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.axes[2].axisBlock[0], 1, configuration.size[0]);
							}
						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

					}
					else {*/
						//didn't transpose 0-1, didn't transpose 1-2
				if (configuration.performR2C == true) {
					for (uint32_t j = 0; j < configuration.numberBatches; j++) {
						for (int l = localFFTPlan.numSupportAxisUploads[1]-1; l >=0; l--) {
							VkFFTAxis* axis = &localFFTPlan.supportAxes[1][l];
							axis->pushConstants.batch = j;
							for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
								axis->pushConstants.coordinate = i;

								vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
								vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
								vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
								vkCmdDispatch(commandBuffer, configuration.size[1] / axis->axisBlock[0] * configuration.size[2] / axis->specializationConstants.fftDim, 1, 1);

							}
							if (l >0)
								vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

						}
					}
				}
				
				for (uint32_t j = 0; j < configuration.numberBatches; j++) {
					for (int l = localFFTPlan.numAxisUploads[2]-1; l >=0; l--) {
						VkFFTAxis* axis = &localFFTPlan.axes[2][l];
						axis->pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							axis->pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
							if (configuration.performR2C == true)
								vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[2] / axis->specializationConstants.fftDim, 1, configuration.size[1]);
							else
								vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[2] / axis->specializationConstants.fftDim, 1, configuration.size[1]);
						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

					}
				}
				
					//}
				//}
				//transpose 1-2, after 0-1
				/*if (configuration.performTranspose[1]) {
					for (uint32_t j = 0; j < configuration.numberBatches; j++) {
						localFFTPlan.transpose[1].pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							localFFTPlan.transpose[1].pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, localFFTPlan.transpose[1].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.transpose[1].pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.transpose[1].pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.transpose[1].pipelineLayout, 0, 1, &localFFTPlan.transpose[1].descriptorSet, 0, NULL);
							if (configuration.performR2C == true) {
								if (localFFTPlan.transpose[1].specializationConstants.ratioDirection)
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.transpose[1].transposeBlock[0], configuration.size[2] / localFFTPlan.transpose[1].transposeBlock[1], (configuration.size[0] / 2 + 1) / localFFTPlan.transpose[1].transposeBlock[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[2] / localFFTPlan.transpose[1].transposeBlock[0], configuration.size[1] / localFFTPlan.transpose[1].transposeBlock[1], (configuration.size[0] / 2 + 1) / localFFTPlan.transpose[1].transposeBlock[2]);

							}
							else {
								if (localFFTPlan.transpose[1].specializationConstants.ratioDirection)
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.transpose[1].transposeBlock[0], configuration.size[2] / localFFTPlan.transpose[1].transposeBlock[1], configuration.size[0] / localFFTPlan.transpose[1].transposeBlock[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[2] / localFFTPlan.transpose[1].transposeBlock[0], configuration.size[1] / localFFTPlan.transpose[1].transposeBlock[1], configuration.size[0] / localFFTPlan.transpose[1].transposeBlock[2]);

							}
						}
					}
					vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

				}*/

			}
			if (configuration.FFTdim > 1) {

				//FFT axis 1
				/*if (configuration.performTranspose[0]) {
					for (uint32_t j = 0; j < configuration.numberBatches; j++) {
						localFFTPlan.axes[1].pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							localFFTPlan.axes[1].pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, localFFTPlan.axes[1].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.axes[1].pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[1].pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.axes[1].pipelineLayout, 0, 1, &localFFTPlan.axes[1].descriptorSet, 0, NULL);
							if (configuration.performR2C == true)
								vkCmdDispatch(commandBuffer, 1, configuration.size[0] / 2 / localFFTPlan.axes[1].axisBlock[1] + 1, configuration.size[2] / localFFTPlan.axes[1].axisBlock[2]);
							else
								vkCmdDispatch(commandBuffer, 1, configuration.size[0] / localFFTPlan.axes[1].axisBlock[1], configuration.size[2] / localFFTPlan.axes[1].axisBlock[2]);

						}
					}
					vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

				}
				else {*/
				
				if (configuration.performR2C == true) {
					for (uint32_t j = 0; j < configuration.numberBatches; j++) {
						for (int l = localFFTPlan.numSupportAxisUploads[0]-1; l >= 0; l--) {
							VkFFTAxis* axis = &localFFTPlan.supportAxes[0][l];
							axis->pushConstants.batch = j;
							for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
								axis->pushConstants.coordinate = i;
								vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
								vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
								vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
								if (l == 0) {
									if (configuration.performZeropadding[2]) {
										vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
									}
									else {
										vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);
									}
								}
								else {
									if (configuration.performZeropadding[2]) {
										vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim / axis->axisBlock[0], 1, ceil(configuration.size[2] / 2.0));
									}
									else {
										vkCmdDispatch(commandBuffer, configuration.size[1] / axis->specializationConstants.fftDim / axis->axisBlock[0], 1, configuration.size[2]);
									}
								}
							}
							if (l >= 0)
								vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

						}
					}
				}
				for (uint32_t j = 0; j < configuration.numberBatches; j++) {
					for (int l = localFFTPlan.numAxisUploads[1]-1; l >= 0; l--) {
						VkFFTAxis* axis = &localFFTPlan.axes[1][l];
						axis->pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							axis->pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
							if (configuration.performZeropadding[2]) {
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
								else
									vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, ceil(configuration.size[2] / 2.0));
							}
							else {
								if (configuration.performR2C == true)
									vkCmdDispatch(commandBuffer, ceil(configuration.size[0] / 2.0) / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[0] / axis->axisBlock[0] * configuration.size[1] / axis->specializationConstants.fftDim, 1, configuration.size[2]);

							}
						}
						vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

					}
				}
				//}

				// transpose 0 - 1, if needed
				/*if (configuration.performTranspose[0]) {
					for (uint32_t j = 0; j < configuration.numberBatches; j++) {
						localFFTPlan.transpose[0].pushConstants.batch = j;
						for (uint32_t i = 0; i < configuration.coordinateFeatures; i++) {
							localFFTPlan.transpose[0].pushConstants.coordinate = i;
							vkCmdPushConstants(commandBuffer, localFFTPlan.transpose[0].pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &localFFTPlan.transpose[0].pushConstants);
							vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.transpose[0].pipeline);
							vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, localFFTPlan.transpose[0].pipelineLayout, 0, 1, &localFFTPlan.transpose[0].descriptorSet, 0, NULL);
							if (configuration.performR2C == true) {
								if (localFFTPlan.transpose[0].specializationConstants.ratioDirection)
									vkCmdDispatch(commandBuffer, configuration.size[0] / 2 / localFFTPlan.transpose[0].transposeBlock[0], configuration.size[1] / localFFTPlan.transpose[0].transposeBlock[1], configuration.size[2] / localFFTPlan.transpose[0].transposeBlock[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.transpose[0].transposeBlock[0], configuration.size[0] / 2 / localFFTPlan.transpose[0].transposeBlock[1], configuration.size[2] / localFFTPlan.transpose[0].transposeBlock[2]);

							}
							else {
								if (localFFTPlan.transpose[0].specializationConstants.ratioDirection)
									vkCmdDispatch(commandBuffer, configuration.size[0] / localFFTPlan.transpose[0].transposeBlock[0], configuration.size[1] / localFFTPlan.transpose[0].transposeBlock[1], configuration.size[2] / localFFTPlan.transpose[0].transposeBlock[2]);
								else
									vkCmdDispatch(commandBuffer, configuration.size[1] / localFFTPlan.transpose[0].transposeBlock[0], configuration.size[0] / localFFTPlan.transpose[0].transposeBlock[1], configuration.size[2] / localFFTPlan.transpose[0].transposeBlock[2]);

							}
						}
					}
					vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

				}*/

			}
			//FFT axis 0
			for (uint32_t j = 0; j < configuration.numberBatches; j++) {
				for (int l = localFFTPlan.numAxisUploads[0]-1; l >=0; l--) {
					VkFFTAxis* axis = &localFFTPlan.axes[0][l];
					axis->pushConstants.batch = j;
					uint32_t maxCoordinate = ((configuration.matrixConvolution) > 1 && (configuration.performConvolution) && (configuration.FFTdim == 1)) ? 1 : configuration.coordinateFeatures;
					for (uint32_t i = 0; i < maxCoordinate; i++) {
						axis->pushConstants.coordinate = i;
						vkCmdPushConstants(commandBuffer, axis->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkFFTPushConstantsLayout), &axis->pushConstants);
						vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipeline);
						vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, axis->pipelineLayout, 0, 1, &axis->descriptorSet, 0, NULL);
						if (l == 0) {
							if (configuration.performZeropadding[1]) {
								if (configuration.performZeropadding[2]) {

									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0 / 2.0), ceil(configuration.size[2] / 2.0));
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0), ceil(configuration.size[2] / 2.0));
								}
								else {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0 / 2.0), configuration.size[2]);
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0), configuration.size[2]);
								}
							}
							else {
								if (configuration.performZeropadding[2]) {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0), ceil(configuration.size[2] / 2.0));
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, configuration.size[1], ceil(configuration.size[2] / 2.0));
								}
								else {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, ceil(configuration.size[1] / 2.0), configuration.size[2]);
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim, configuration.size[1], configuration.size[2]);
								}
							}
						}
						else {
							if (configuration.performZeropadding[1]) {
								if (configuration.performZeropadding[2]) {

									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0 / 2.0), ceil(configuration.size[2] / 2.0));
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0), ceil(configuration.size[2] / 2.0));
								}
								else {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0 / 2.0), configuration.size[2]);
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0), configuration.size[2]);
								}
							}
							else {
								if (configuration.performZeropadding[2]) {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0), ceil(configuration.size[2] / 2.0));
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], configuration.size[1], ceil(configuration.size[2] / 2.0));
								}
								else {
									if (configuration.performR2C == true)
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], ceil(configuration.size[1] / 2.0), configuration.size[2]);
									else
										vkCmdDispatch(commandBuffer, configuration.size[0] / axis->specializationConstants.fftDim / axis->axisBlock[0], configuration.size[1], configuration.size[2]);
								}
							}
						}
					}
					vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

				}
			}
			

		}

	}
	void deleteVulkanFFT() {
		for (uint32_t i = 0; i < configuration.FFTdim; i++) {
			for (uint32_t j = 0; j < localFFTPlan.numAxisUploads[i]; j++)
				deleteAxis(&localFFTPlan.axes[i][j]);
		}

		for (uint32_t i = 0; i < configuration.FFTdim-1; i++) {
			if (configuration.performTranspose[i])
				deleteTranspose(&localFFTPlan.transpose[i]);
			else {
				for (uint32_t j = 0; j < localFFTPlan.numSupportAxisUploads[i]; j++) 
					deleteAxis(&localFFTPlan.supportAxes[i][j]);
			}
		}
		if (configuration.performConvolution) {
			for (uint32_t i = 0; i < configuration.FFTdim; i++) {
				for (uint32_t j = 0; j < localFFTPlan_inverse_convolution.numAxisUploads[i]; j++)
					deleteAxis(&localFFTPlan_inverse_convolution.axes[i][j]);
			}
			for (uint32_t i = 0; i < configuration.FFTdim - 1; i++) {
				if (configuration.performTranspose[i])
					deleteTranspose(&localFFTPlan_inverse_convolution.transpose[i]);
				else {
					for (uint32_t j = 0; j < localFFTPlan_inverse_convolution.numSupportAxisUploads[i]; j++)
						deleteAxis(&localFFTPlan_inverse_convolution.supportAxes[i][j]);
				}
			}
		}
	}
};
