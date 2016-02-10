/* Copyright 2009-2015 CoReNum, INSA-Lyon
 * 
 * This file is part of libcrn.
 * 
 * libcrn is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * libcrn is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with libcrn.  If not, see <http://www.gnu.org/licenses/>.
 * 
 * file: CRNBlockTreeExtractorWordsFromProjection.cpp
 * \author Jean DUONG
 */

#include <CRNFeature/CRNBlockTreeExtractorWordsFromProjection.h>
#include <CRNImage/CRNImageBW.h>
#include <CRNMath/CRNUnivariateGaussianMixture.h>
#include <CRNMath/CRNMatrixDouble.h>
#include <CRNMath/CRNSquareMatrixDouble.h>
#include <CRNStatistics/CRNHistogram.h>
#include <CRNData/CRNInt.h>
#include <CRNBlock.h>
#include <limits>
#include <CRNData/CRNDataFactory.h>
#include <CRNUtils/CRNXml.h>
#include <CRNi18n.h>

using namespace crn;

/*! Computes a child-tree on a block 
 * \throws	ExceptionInvalidArgument	null block
 * \param[in] b text line from which words will be extracted
 */
void BlockTreeExtractorWordsFromProjection::Extract(Block &b)
{
	if (!b.HasTree(connectedComponentTreeName))
	{
		// Check if block contains a tree of connected components
		// If not, perform a component extraction
		b.ExtractCC(connectedComponentTreeName);
		
		// Drop connected components with width AND height smaller than 2
		b.FilterMinAnd(connectedComponentTreeName, 2, 2);
	}

	size_t nbCCs = b.GetNbChildren(connectedComponentTreeName);

	if (nbCCs == 0)
	{
		// No connected component in this block => no word !!!
		// Nothing to be done but no error. Just return.
		return;
	}

	/*** General case ***/
	
	// Step 1 : vertical projection
	
	auto vProj = VerticalProjection(*b.GetBW());
	
	// Step 2 : find white streams (start points, end points, lengths) 
	//          in profile
	
	std::vector< size_t > whiteStreamStartPoints;
	std::vector< size_t > whiteStreamEndPoints;
	std::vector< double > whiteStreamLengths;

	bool inWhiteStream = (vProj.GetBin(0) == 0);
	size_t n = vProj.Size();
	double l = 0.0;
	
	if (inWhiteStream)
	{
		l = 1.0;
	}
	
	for (size_t k = 1; k < n; k++)
	{
		if (vProj.GetBin(k) == 0)
		{
			// Case 1 : void bin
			
			if (!inWhiteStream)
			{
				// Case 1.1 : begin a new white stream
				inWhiteStream = true;
				whiteStreamStartPoints.push_back(k);
				l = 1.0;
			}
			else
			{
				// Case 1.2 : continue white stream
				l += 1.0;
			}
		}
		else
		{
			// Case 2 : non-void bin
			
			if (inWhiteStream)
			{
				// Case 2.1 : run out of a white stream
				inWhiteStream = false;
				whiteStreamEndPoints.push_back(k - 1);
				whiteStreamLengths.push_back(l);
			}
			
		}
	}
	
	// Step 3 : statistical calculus
	
	// Pattern matrix to store white stream lengths
	size_t nbWhiteStreams = whiteStreamLengths.size();
	SMatrixDouble data = std::make_shared<MatrixDouble>(nbWhiteStreams, 1, 0.0);
	
	for (size_t k = 0; k < nbWhiteStreams; k++)
	{
		data->At(k, 0) = whiteStreamLengths[k];
	}
	
	double mu = data->MakeColumnMeans().At(0, 0);
	double rho = data->MakeCovariance().At(0, 0);
	
	// Create univariate gaussian mixture
	SUnivariateGaussianMixture ugm = std::make_shared<UnivariateGaussianMixture>();
	
	// Try EM optimisation and compute likelihood with 1 PDF hypothesis
	ugm->EM(*data, 1);
	double mlle1 = ugm->MLLE(*data);
	// Try EM optimisation and compute likelihood with 2 PDF hypothesis
	ugm->EM(*data, 2);
	double mlle2 = ugm->MLLE(*data);

	// Get high and low values means of PDFs in mixture
	double m_low = ugm->GetMember(0).GetMean();
	double m_high = ugm->GetMember(1).GetMean();
	
	// Check and swap values if needed
	if (m_low > m_high)
	{
		double tmp = m_high;
		
		m_high = m_low;
		m_low = tmp;
	}
	
	// Step 4 : cases for box splitting

	int nb_words = 1;
	double avg_between_word_spacing = 0;

	Rect bb = b.GetAbsoluteBBox();
	int leftSide = bb.GetLeft();
	int rightSide = bb.GetRight();
	int topSide = bb.GetTop();
	int bottomSide = bb.GetBottom();
	
	if (mlle2 == std::numeric_limits<double>::infinity())
	{
		// Case 1 : one member has null variance in the 2 PDF mixture
		// i.e. the PDF was generated by a single data
		
		if (rho == 0.0)
		{
			// Case 1.1 : all values equal in data set
			// No need to spit. Block simply added as is.
			b.AddChildAbsolute(GetTreeName(), b.GetAbsoluteBBox());
		}
		else
		{
			// Case 1.2 : one outlier value in data set
			
			double outlier;
			
			if (ugm->GetMember(0).GetVariance() == 0.0)
			{
				outlier = ugm->GetMember(0).GetMean();
			}
			else
			{
				outlier = ugm->GetMember(1).GetMean();
			}
			
			if (outlier < mu)
			{
				// Case 1.2.1 : outlier is smaller than average gap lengths
				// No need to spit. Block simply added as is.
				b.AddChildAbsolute(GetTreeName(), b.GetAbsoluteBBox());
			}
			else
			{
				// Case 1.2.2 : outlier is greater than average gap lengths
				// Split in two sub-blocks
				int k = 0;
				
				while (whiteStreamLengths[k] != outlier)
				{
					k++;
				}
				
				b.AddChildAbsolute(GetTreeName(), Rect(leftSide, topSide, leftSide + int(whiteStreamStartPoints[k]) - 1, bottomSide));
				b.AddChildAbsolute(GetTreeName(), Rect(leftSide + int(whiteStreamEndPoints[k]) + 1, topSide, rightSide, bottomSide));
				
				avg_between_word_spacing = outlier;
				nb_words = 2;
			}
		}
	}
	else
	{
		// Case 2 : no member has null variance in the 2 PDF mixture
		
		if (mlle2 > mlle1)
		{
			// Case 2.1 : data set likely comes from two distributions
			// Catch high values in data set and split block at
			// gaps corresponding to large white streams
			
			// Check white streams and keep the long ones
			std::vector< size_t > 	gapStartPoints;
			std::vector< size_t > 	gapEndPoints;
			std::vector< double > 	gapWidths;

			for (size_t k = 0; k < nbWhiteStreams; k++)
			{
				double lk = whiteStreamLengths[k];
				
				if (fabs(lk - m_high) < fabs(lk - m_low))
				{
					gapStartPoints.push_back(whiteStreamStartPoints[k]);
					gapEndPoints.push_back(whiteStreamEndPoints[k]);
					gapWidths.push_back(whiteStreamLengths[k]);
				}
			}
			
			size_t nbGaps = gapStartPoints.size();
			
			if (gapStartPoints[0] > 0)
			{
				b.AddChildAbsolute(GetTreeName(), Rect(leftSide, topSide, leftSide + int(gapStartPoints[0]) - 1, bottomSide));
			}
			
			for (size_t k = 1; k < nbGaps; k++)
			{
				b.AddChildAbsolute(GetTreeName(), Rect(leftSide + int(gapEndPoints[k - 1]) + 1, topSide, leftSide + int(gapStartPoints[k]) - 1, bottomSide));
				avg_between_word_spacing += gapWidths[k];
				nb_words++;
			}
			
			if (int(gapEndPoints[nbGaps - 1]) < rightSide)
			{
				b.AddChildAbsolute(GetTreeName(), Rect(leftSide + int(gapEndPoints[nbGaps - 1]) + 1, topSide, rightSide, bottomSide));
				avg_between_word_spacing += gapWidths[nbGaps - 1];
				nb_words++;
			}
		}
		else
		{
			// Case 2.2 : data set likely comes from a unique distributions
			// No need to spit. Block simply added as is.
			b.AddChildAbsolute(GetTreeName(), b.GetAbsoluteBBox());
		}
		
	}
	
	avg_between_word_spacing /= nb_words;
	
	b.SetUserData(U"nb_words", std::make_shared<Int>(nb_words));
	b.SetUserData(U"avg_between_word_spacing", std::make_shared<Int>((int)(avg_between_word_spacing)));
}

/*****************************************************************************/
/*! 
 * Initializes the object from an XML element. Unsafe. 
 * \throws	ExceptionInvalidArgument	not a FeatureExtractorProjection
 * \throws	ExceptionNotFound	attribute not found
 * \throws	ExceptionDomain	wrong attribute
 */
void BlockTreeExtractorWordsFromProjection::deserialize(xml::Element &el)
{
	if (el.GetName() != GetClassName().CStr())
	{
		throw ExceptionInvalidArgument(StringUTF8("bool BlockTreeExtractorWordsFromProjection::deserialize("
					"xml::Element &el): ") + _("Wrong XML element."));
	}
	StringUTF8 wtn = el.GetAttribute<StringUTF8>("wordTreeName"); // may throw
	StringUTF8 cctn = el.GetAttribute<StringUTF8>("ccTreeName"); // may throw
	wordTreeName = wtn;
	connectedComponentTreeName = cctn;
	
}

/*****************************************************************************/
/*! 
 * Dumps the object to an XML element. Unsafe. 
 */
xml::Element BlockTreeExtractorWordsFromProjection::serialize(xml::Element &parent) const
{
	xml::Element el(parent.PushBackElement(GetClassName().CStr()));
	el.SetAttribute("wordTreeName", wordTreeName.CStr());
	el.SetAttribute("ccTreeName", connectedComponentTreeName.CStr());
	return el;
}


CRN_BEGIN_CLASS_CONSTRUCTOR(BlockTreeExtractorWordsFromProjection)
	CRN_DATA_FACTORY_REGISTER(U"BlockTreeExtractorWordsFromProjection", BlockTreeExtractorWordsFromProjection)
CRN_END_CLASS_CONSTRUCTOR(BlockTreeExtractorWordsFromProjection)


