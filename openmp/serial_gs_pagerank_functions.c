/* ===== INCLUDES ===== */

#include "serial_gs_pagerank_functions.h"
#include <omp.h>
/* ===== CONSTANTS ===== */

const char *ARGUMENT_CONVERGENCE_TOLERANCE = "-c";
const char *ARGUMENT_MAX_ITERATIONS = "-m";
const char *ARGUMENT_DAMPING_FACTOR = "-a";
const char *ARGUMENT_VERBAL_OUTPUT = "-v";
const char *ARGUMENT_OUTPUT_HISTORY = "-h";
const char *ARGUMENT_OUTPUT_FILENAME = "-o";

const int NUMERICAL_BASE = 10;
char *DEFAULT_OUTPUT_FILENAME = "pagerank_output";
const int FILE_READ_BUFFER_SIZE = 4096;

const int CONVERGENCE_CHECK_ITERATION_PERIOD = 3;
const int SPARSITY_INCREASE_ITERATION_PERIOD = 3;

/* ===== FUNCTIONS ===== */

int pagerank(CsrSparseMatrix *transitionMatrix, double **pagerankVector,
	bool *convergenceStatus, Parameters parameters) {
	// Variables declaration
	int iterations = 0, numberOfPages = parameters.numberOfPages;
	double delta, *pagerankDifference, *previousPagerankVector,
	*convergedPagerankVector, *linksFromConvergedPagesPagerankVector;
	CooSparseMatrix linksFromConvergedPages = initCooSparseMatrix();
	bool *convergenceMatrix;

	int P = omp_get_max_threads();
	omp_set_num_threads(P);
	
	// Space allocation
	{
		size_t sizeofDouble = sizeof(double);
		// pagerankDifference used to calculate delta
		pagerankDifference = (double *) malloc(numberOfPages * sizeofDouble);
		// previousPagerankVector holds last iteration's pagerank vector
		previousPagerankVector = (double *) malloc(numberOfPages * sizeofDouble);
		// convergedPagerankVector is the pagerank vector of converged pages only
		convergedPagerankVector = (double *) malloc(numberOfPages * sizeofDouble);
		// linksFromConvergedPagesPagerankVector holds the partial sum of the
		// pagerank vector, that describes effect of the links from converged
		// pages to non converged pages
		linksFromConvergedPagesPagerankVector = (double *) malloc(numberOfPages * sizeofDouble);
		// convergenceMatrix indicates which pages have converged
		convergenceMatrix = (bool *) malloc(numberOfPages * sizeof(bool));
		*convergenceStatus = false;

		// Initialization
		allocMemoryForCoo(&linksFromConvergedPages, transitionMatrix->numberOfNonZeroElements);
		#pragma omp parallel for num_threads(P)
		for (int i=0; i<numberOfPages; ++i) {
			convergedPagerankVector[i] = 0;
			convergenceMatrix[i] = false;
			linksFromConvergedPagesPagerankVector[i] = 0;
		}
	}

	if (parameters.verbose) {
		printf(ANSI_COLOR_YELLOW "\n----- Starting iterations -----\n" ANSI_COLOR_RESET);
	}

	do {
		// Stores previous pagerank vector
		memcpy(previousPagerankVector, *pagerankVector, numberOfPages * sizeof(double));
		
		// Calculates new pagerank vector
		calculateNextPagerank(transitionMatrix, previousPagerankVector,
			pagerankVector, linksFromConvergedPagesPagerankVector,
			convergedPagerankVector, numberOfPages,
			parameters.dampingFactor);

		if (parameters.history) {
			// Outputs pagerank vector to file
			savePagerankToFile(parameters.outputFilename, iterations != 0,
				*pagerankVector, numberOfPages, parameters.realIterations);
		}

		// Periodically checks for convergence
		if (!(iterations % CONVERGENCE_CHECK_ITERATION_PERIOD)) {
			// Builds pagerank vectors difference
			#pragma omp parallel for num_threads(P)
			for (int i=0; i<numberOfPages; ++i) {
				pagerankDifference[i] = (*pagerankVector)[i] - previousPagerankVector[i];
			}
			// Calculates convergence
			delta = vectorNorm(pagerankDifference, numberOfPages);

			if (delta < parameters.convergenceCriterion) {
				// Converged
				*convergenceStatus = true;
			}
		}

		// Periodically increases sparsity
		if (iterations && !(iterations % SPARSITY_INCREASE_ITERATION_PERIOD)) {
			bool *newlyConvergedPages = (bool *) malloc(numberOfPages * sizeof(bool));
			// Checks each individual page for convergence
			#pragma omp parallel for num_threads(P)
			for (int i=0; i<numberOfPages; ++i) {
				double difference = fabs((*pagerankVector)[i] -
					previousPagerankVector[i]) / fabs(previousPagerankVector[i]);

				newlyConvergedPages[i] = false;
				if (!convergenceMatrix[i] && difference < parameters.convergenceCriterion){
					// Page converged
					newlyConvergedPages[i] = true;
					convergenceMatrix[i] = true;
					convergedPagerankVector[i] = (*pagerankVector)[i];
				}
			}
			#pragma omp parallel for num_threads(P)
			for (int i=0; i<numberOfPages; ++i) {
				// Filters newly converged pages
				if (newlyConvergedPages[i] == true) {
					// Checks if this converged page has an out-link to a non converged one
					int rowStartIndex = transitionMatrix->rowCumulativeIndexes[i],
					rowEndIndex = transitionMatrix->rowCumulativeIndexes[i+1];
					if (rowEndIndex > rowStartIndex) {
						// This row (page) has non zero elements (out-links)
						for (int j=rowStartIndex; j<rowEndIndex; ++j) {
							// Checks for links from converged pages to non converged
							int pageLinksTo = transitionMatrix->columnIndexes[j];
							if (convergenceMatrix[pageLinksTo] == false){
								// Link exists, adds element to the vector
								addElement(&linksFromConvergedPages,
									transitionMatrix->values[j], i, pageLinksTo);
							}
						}
					}

					// Increases sparsity of the transition matrix by
					// deleting elements that correspond to converged pages
					zeroOutRow(transitionMatrix, i);
					zeroOutColumn(transitionMatrix, i);

					// Builds the new linksFromConvergedPagesPagerankVector
					cooSparseMatrixVectorMultiplication(linksFromConvergedPages,
						*pagerankVector, &linksFromConvergedPagesPagerankVector,
						numberOfPages);
				}
			}
			free(newlyConvergedPages);
		}

		++iterations;
		// Outputs information about this iteration
		if (iterations%2) {
			printf(ANSI_COLOR_BLUE "Iteration %d: delta = %f\n" ANSI_COLOR_RESET, iterations, delta);
		} else {
			printf(ANSI_COLOR_CYAN "Iteration %d: delta = %f\n" ANSI_COLOR_RESET, iterations, delta);
		}
	} while (!*convergenceStatus && (parameters.maxIterations == 0 ||
		iterations < parameters.maxIterations));
	parameters.realIterations = iterations;
	if (!parameters.history) {
		// Outputs last pagerank vector to file
		savePagerankToFile(parameters.outputFilename, false, *pagerankVector,
			numberOfPages, parameters.realIterations);
	}

	// Frees memory
	free(pagerankDifference);
	free(previousPagerankVector);
	free(convergedPagerankVector);
	free(linksFromConvergedPagesPagerankVector);
	free(convergenceMatrix);
	destroyCooSparseMatrix(&linksFromConvergedPages);
	
	return iterations;
}

/*
 * initialize allocates required memory for arrays, reads the web graph from the
 * from the file and creates the initial transition probability distribution
 * matrix.
*/
void initialize(CsrSparseMatrix *transitionMatrix,
	double **pagerankVector, Parameters *parameters) {

	// Reads web graph from file
	if ((*parameters).verbose) {
		printf(ANSI_COLOR_YELLOW "----- Reading graph from file -----\n" ANSI_COLOR_RESET);
	}
	generateNormalizedTransitionMatrixFromFile(transitionMatrix, parameters);

	// Outputs the algorithm parameters to the console
	if ((*parameters).verbose) {
		printf(ANSI_COLOR_YELLOW "\n----- Running with parameters -----\n" ANSI_COLOR_RESET\
			"Number of pages: %d", (*parameters).numberOfPages);
		if (!(*parameters).maxIterations) {
			printf("\nMaximum number of iterations: inf");
		} else {
			printf("\nMaximum number of iterations: %d", (*parameters).maxIterations);
		}
		printf("\nConvergence criterion: %f" \
			"\nDamping factor: %f" \
			"\nGraph filename: %s\n", (*parameters).convergenceCriterion,
			(*parameters).dampingFactor, (*parameters).graphFilename);
	}
	(*parameters).realIterations = 0;
	// Allocates memory for the pagerank vector
	(*pagerankVector) = (double *) malloc((*parameters).numberOfPages * sizeof(double));
	double webUniformProbability = 1. / (*parameters).numberOfPages;
	for (int i=0; i<(*parameters).numberOfPages; ++i) {
		(*pagerankVector)[i] = webUniformProbability;
	}
}

// ==================== MATH UTILS ====================

/*
 * calculateNextPagerank calculates the product of the multiplication
 * between a matrix and the a vector in a cheap way.
*/
void calculateNextPagerank(CsrSparseMatrix *transitionMatrix,
	double *previousPagerankVector, double **pagerankVector,
	double *linksFromConvergedPagesPagerankVector,
	double *convergedPagerankVector, int vectorSize, double dampingFactor) {
	// Calculates the web uniform probability once.
	
	
	double webUniformProbability = 1. / vectorSize;

	csrSparseMatrixVectorMultiplication(*transitionMatrix, previousPagerankVector,
		pagerankVector, vectorSize);
	#pragma omp parallel for
	for (int i=0; i<vectorSize; ++i) {
		(*pagerankVector)[i] = dampingFactor * (*pagerankVector)[i];
	}

	double normDifference = vectorNorm(previousPagerankVector, vectorSize) -
	vectorNorm(*pagerankVector, vectorSize);
	#pragma omp parallel for
	for (int i=0; i<vectorSize; ++i) {
		//(*pagerankVector)[i] += normDifference * webUniformProbability +
		//linksFromConvergedPagesPagerankVector[i] + convergedPagerankVector[i];
		(*pagerankVector)[i] += 0.5*normDifference* webUniformProbability +linksFromConvergedPagesPagerankVector[i] + convergedPagerankVector[i];
	}
}

/*
 * vectorNorm calculates the first norm of a vector.
*/
double vectorNorm(double *vector, int vectorSize) {
	double norm = 0.;

	for (int i=0; i<vectorSize; ++i) {
		norm += fabs(vector[i]);
	}

	return norm;
}

// ==================== PROGRAM INPUT AND OUTPUT UTILS ====================

/*
 * parseArguments parses the command line arguments given by the user.
*/
void parseArguments(int argumentCount, char **argumentVector, Parameters *parameters) {
	if (argumentCount < 2 || argumentCount > 10) {
		validUsage(argumentVector[0]);
	}

	(*parameters).numberOfPages = 0;
	(*parameters).maxIterations = 0;
	(*parameters).convergenceCriterion = 1;
	(*parameters).dampingFactor = 0.85;
	(*parameters).verbose = false;
	(*parameters).history = false;
	(*parameters).outputFilename = DEFAULT_OUTPUT_FILENAME;

	char *endPointer;
	int argumentIndex = 1;

	while (argumentIndex < argumentCount) {
		if (!strcmp(argumentVector[argumentIndex], ARGUMENT_CONVERGENCE_TOLERANCE)) {
			argumentIndex = checkIncrement(argumentIndex, argumentCount, argumentVector[0]);

			double convergenceInput = strtod(argumentVector[argumentIndex], &endPointer);
			if (convergenceInput == 0) {
				printf("Invalid convergence argument\n");
				exit(EXIT_FAILURE);
			}
			(*parameters).convergenceCriterion = convergenceInput;
		} else if (!strcmp(argumentVector[argumentIndex], ARGUMENT_MAX_ITERATIONS)) {
			argumentIndex = checkIncrement(argumentIndex, argumentCount, argumentVector[0]);

			size_t iterationsInput = strtol(argumentVector[argumentIndex], &endPointer, NUMERICAL_BASE);
			if (iterationsInput == 0 && endPointer) {
				printf("Invalid iterations argument\n");
				exit(EXIT_FAILURE);
			}
			(*parameters).maxIterations = iterationsInput;
		} else if (!strcmp(argumentVector[argumentIndex], ARGUMENT_DAMPING_FACTOR)) {
			argumentIndex = checkIncrement(argumentIndex, argumentCount, argumentVector[0]);

			double alphaInput = strtod(argumentVector[argumentIndex], &endPointer);
			if ((alphaInput == 0 || alphaInput > 1) && endPointer) {
				printf("Invalid alpha argument\n");
				exit(EXIT_FAILURE);
			}
			(*parameters).dampingFactor = alphaInput;
		} else if (!strcmp(argumentVector[argumentIndex], ARGUMENT_VERBAL_OUTPUT)) {
			(*parameters).verbose = true;
		} else if (!strcmp(argumentVector[argumentIndex], ARGUMENT_OUTPUT_HISTORY)) {
			(*parameters).history = true;
		} else if (!strcmp(argumentVector[argumentIndex], ARGUMENT_OUTPUT_FILENAME)) {
			argumentIndex = checkIncrement(argumentIndex, argumentCount, argumentVector[0]);

			if (fopen(argumentVector[argumentIndex], "w") == NULL) {
				printf("Invalid output filename. Reverting to default.\n");
				continue;
			}
			(*parameters).outputFilename = argumentVector[argumentIndex];
		} else if (argumentIndex == argumentCount - 1) {
			(*parameters).graphFilename = argumentVector[argumentIndex];
		} else {
			validUsage(argumentVector[0]);
			exit(EXIT_FAILURE);
		}
		++argumentIndex;
	}
}

/*
 * readGraphFromFile loads the file supplied in the command line arguments to an
 * array (directedWebGraph) that represents the graph.
*/
void generateNormalizedTransitionMatrixFromFile(CsrSparseMatrix *transitionMatrix,
	Parameters *parameters){
	FILE *graphFile;

	// Opens the file for reading
	graphFile = fopen((*parameters).graphFilename, "r+");
	if (!graphFile) {
		printf("Error opening file \n");
		exit(EXIT_FAILURE);
	}

	char buffer[FILE_READ_BUFFER_SIZE];
	char *readResult;
	// Skips the first two lines
	readResult = fgets(buffer, FILE_READ_BUFFER_SIZE, graphFile);
	readResult = fgets(buffer, FILE_READ_BUFFER_SIZE, graphFile);
	if (readResult == NULL) {
		printf("Error while reading from the file. Does the file have the correct format?\n");
		exit(EXIT_FAILURE);
	}

	// Third line contains the numbers of nodes and edges
	int numberOfNodes = 0, numberOfEdges = 0;

	readResult = fgets(buffer, FILE_READ_BUFFER_SIZE, graphFile);
	if (readResult == NULL) {
		printf("Error while reading from the file. Does the file have the correct format?\n");
		exit(EXIT_FAILURE);
	}

	// Parses the number of nodes and number of edges
	{
		// Splits string to whitespace
		char *token = strtok(buffer, " ");
		bool nextIsNodes = false, nextIsEdges = false;

		while (token != NULL) {
			if (strcmp(token, "Nodes:") == 0) {
				nextIsNodes = true;
			} else if (nextIsNodes) {
				numberOfNodes = atoi(token);
				nextIsNodes = false;
			} else if (strcmp(token, "Edges:") == 0) {
				nextIsEdges = true;
			} else if (nextIsEdges) {
				numberOfEdges = atoi(token);
				break;
			}

			// Gets next string token
			token = strtok (NULL, " ,.-");
		}
	}

	if ((*parameters).verbose) {
		printf("File claims number of pages is: %d\nThe number of edges is: %d\n",
			numberOfNodes, numberOfEdges);
	}

	// Skips the fourth line
	readResult = fgets(buffer, 512, graphFile);
	if (readResult == NULL) {
		printf("Error while reading from the file. Does the file have the correct format?\n");
		exit(EXIT_FAILURE);
	}

	
	int maxPageIndex = 0;
	CooSparseMatrix tempMatrix = initCooSparseMatrix();
	allocMemoryForCoo(&tempMatrix, numberOfEdges);

	for (int i=0; i<numberOfEdges; i++) {
		int fileFrom = 0, fileTo = 0;
		if (!fscanf(graphFile, "%d %d", &fileFrom, &fileTo)) {
			break;
		}

		if (fileFrom > maxPageIndex) {
			maxPageIndex = fileFrom;
		}
		if (fileTo > maxPageIndex) {
			maxPageIndex = fileTo;
		}
		addElement(&tempMatrix, 1, fileFrom, fileTo);
	}

	if ((*parameters).verbose) {
		printf("Max page index found is: %d\n", maxPageIndex);
	}
	(*parameters).numberOfPages = maxPageIndex + 1;

	// Calculates the outdegree of each page and assigns the uniform probability
	// of transition to the elements of the corresponding row

	int* pageOutdegree = malloc((*parameters).numberOfPages*sizeof(int));
	for (int i=0; i<(*parameters).numberOfPages; ++i){
		pageOutdegree[i] = 0;
	}

	
	for (int i=0; i<numberOfEdges; ++i) {
		int currentRow = tempMatrix.elements[i]->rowIndex;

				if (currentRow == tempMatrix.elements[i]->rowIndex) {
					++pageOutdegree[currentRow];
				} 
	
			
	}

	for (int i=0; i<tempMatrix.size; ++i) {
		tempMatrix.elements[i]->value = 1./pageOutdegree[tempMatrix.elements[i]->rowIndex];
	}
	
	// Transposes the temporary transition matrix (P^T).
	transposeSparseMatrix(&tempMatrix);
	allocMemoryForCsr(transitionMatrix, numberOfEdges);
	// Transforms the temporary COO matrix to the desired CSR format
	transformToCSR(tempMatrix, transitionMatrix);
	//printCsrSparseMatrix(*transitionMatrix);
	destroyCooSparseMatrix(&tempMatrix);

	fclose(graphFile);
}

/*
 * validUsage outputs a message to the console that informs the user of the
 * correct (valid) way to use the program.
*/
void validUsage(char *programName) {
	printf("%s [-c convergence_criterion] [-m max_iterations] [-a alpha] [-v] [-h] [-o output_filename] <graph_file>" \
		"\n-c convergence_criterion" \
		"\n\tthe convergence tolerance criterion" \
		"\n-m max_iterations" \
		"\n\tmaximum number of iterations to perform" \
		"\n-a alpha" \
		"\n\tthe damping factor" \
		"\n-v enable verbal output" \
		"\n-h enable history output to file" \
		"\n-o output_filename" \
		"\n\tfilename and path for the output" \
		"\n", programName);
	exit(EXIT_FAILURE);
}

/*
 * checkIncrement is a helper function for parseArguments function.
*/
int checkIncrement(int previousIndex, int maxIndex, char *programName) {
	if (previousIndex == maxIndex) {
		validUsage(programName);
		exit(EXIT_FAILURE);
	}
	return ++previousIndex;
}

void savePagerankToFile(char *filename, bool append, double *pagerankVector,
	int vectorSize, int realIterations) {
	FILE *outputFile;

	if (append) {
		outputFile = fopen(filename, "a");
	} else {
		outputFile = fopen(filename, "w");
	}

	if (outputFile == NULL) {
		printf("Error while opening the output file.\n");
		return;
	}
	//Save numberofPages and convergence time
	
	for (int i=0; i<vectorSize; ++i) {
		fprintf(outputFile, "%f ", pagerankVector[i]);
	}
	fprintf(outputFile, "\n");
	//fprintf(outputFile, "%d\t", vectorSize);
	//fprintf(outputFile, "%d\t", realIterations);
	fclose(outputFile);
}