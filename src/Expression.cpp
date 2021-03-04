//
//  Expression.cpp
//  IntervalTree
//
//  Created by Aaron Graubert on 8/2/17.
//  Copyright © 2017 Aaron Graubert. All rights reserved.
//

#include "Expression.h"
#include <algorithm>

using std::vector;
using std::list;
using std::map;
using std::string;
using std::set;
using std::cout;
using std::endl;
using std::pair;


namespace rnaseqc {
    
    const set<string> blacklistedGlobins = {"HBA1", "HBA2", "HBB", "HBD", "HBG1", "HBG2", "HBE1", "HBM", "HBQ1", "HBZ", "HBBP1", "HBZP1"};
    //this actually is the legacy version, but it works out the same and makes alignment size math a little easier
    unsigned int extractBlocks(Alignment &alignment, vector<Feature> &blocks, chrom chr, bool legacy)
    {
        //parse the cigar string and populate the provided vector with each block of the read
        const SeqLib::Cigar cigar = alignment.GetCigar();
        const unsigned long cigarLen = cigar.size();
        coord start = alignment.Position() + 1;
        unsigned int alignedSize = 0;
        for (unsigned int i = 0; i < cigarLen; ++i)
        {
            SeqLib::CigarField current = cigar[i];
            Feature block;
            switch(current.Type())
            {
                case 'M':
                case '=':
                case 'X':
                    //M, =, and X blocks are aligned, so push back this block
                    block.start = start;
                    block.chromosome = chr;
                    block.end = start + current.Length(); //1-based, closed
                    block.strand = alignment.ReverseFlag() ? Strand::Reverse : Strand::Forward;
                    blocks.push_back(block);
                    alignedSize += current.Length();
                case 'N':
                case 'D':
                    //M, =, X, N, and D blocks all advance the start position of the next block
                    start += current.Length();
                case 'H':
                case 'P':
                    //            case 'S':
                case 'I':
                    break;
                case 'S':
                    if (legacy) alignedSize += current.Length();
                    break;
                default:
                    std::cerr << "Bad cigar operation: " << current.Type() << " " << alignment.CigarString() <<  endl;
                    throw std::invalid_argument("Unrecognized Cigar Op ");
            }
        }
        return alignedSize;
    }
    
    void trimFeatures(Alignment &alignment, list<Feature> &features)
    {
        //trim intervals upstream of this block
        //Since alignments are sorted, if an alignment occurs beyond any features, these features can be dropped
        while (!features.empty() && features.front().end < alignment.Position())
        {
            if (features.front().type == FeatureType::Gene) fragmentTracker.erase(features.front().feature_id);
            features.pop_front();
        }
    }
    
    void trimFeatures(Alignment &alignment, list<Feature> &features, BaseCoverage &coverage)
    {
        //trim intervals upstream of this block
        //Since alignments are sorted, if an alignment occurs beyond any features, these features can be dropped
        while (!features.empty() && features.front().end < alignment.Position())
        {
            if (features.front().type == FeatureType::Gene)
            {
                coverage.compute(features.front()); //Once this gene leaves the search window, compute coverage
                fragmentTracker.erase(features.front().feature_id);
            }
            features.pop_front();
        }
    }
    
    // After we switch chromosomes, just drop all the remaining features from the previous chromosome
    void dropFeatures(std::list<Feature> &features, BaseCoverage &coverage)
    {
        for (auto feat = features.begin(); feat != features.end(); ++feat) if (feat->type == FeatureType::Gene) {
            coverage.compute(*feat);
            fragmentTracker.erase(feat->feature_id);
        }
        features.clear();
    }
    
    // Get the list of features that this aligned segment intersects
    list<Feature>* intersectBlock(Feature &block, list<Feature> &features)
    {
        list<Feature> *output = new list<Feature>();
        //since we've trimmed the beginning of the features, we start from the new beginning here
        //There should be little overhead (at most ~1 gene worth of exons on either end of the block)
        for (auto current = features.begin(); current != features.end() && current->start <= block.end; ++current)
        {
            //check that the current feature actually intersects the current block of the current alignment
            if (intersectInterval(block, *current)) output->push_back(*current);
        }
        return output;
    }
    
    Strand feature_strand(Alignment &alignment, Strand orientation)
    {
        if (orientation == Strand::Unknown) return orientation;
        bool target = alignment.ReverseFlag();
        if ((orientation == Strand::Forward) ^ alignment.FirstFlag()) target = !target;
        return target ? Strand::Reverse : Strand::Forward;
    }
    
    // Legacy version of standard alignment metrics
    // This code is really inefficient, but it's a faithful replication of the original code
    void legacyExonAlignmentMetrics(unsigned int SPLIT_DISTANCE, map<chrom, list<Feature>> &features, Metrics &counter, vector<Feature> &blocks, Alignment &alignment, SeqLib::HeaderSequenceVector &sequenceTable, unsigned int length, Strand orientation, BaseCoverage &baseCoverage, const bool highQuality, const bool singleEnd)
    {
        string chrName = sequenceTable[alignment.ChrID()].Name;
        chrom chr = chromosomeMap(chrName); //generate the chromosome shorthand name
        //check for split reads by iterating over all the blocks of this read
        //    cout << "~" << alignment.Qname();
        bool split = false;
        long long lastEnd = -1; // used for split read detection
        for(auto block = blocks.begin(); block != blocks.end(); ++block)
        {
            if (lastEnd > 0 && !split) split = (block->start - lastEnd) > SPLIT_DISTANCE - 1;
            lastEnd = block->end;
        }
        
        //Bamtools uses 0-based indexing because it's the 'norm' in computer science, even though sams are 1-based
        Feature current; //current block of the alignment (used while iterating)
        current.start = alignment.Position()+1; //0-based + 1 == 1-based
        current.end = alignment.PositionEnd(); //0-based, open == 1-based, closed
        
        list<Feature> *results = intersectBlock(current, features[chr]);
        
        vector<set<string> > genes; //each set is the set of genes intersected by the current block (one set per block)
        bool intragenic = false, transcriptPlus = false, transcriptMinus = false, ribosomal = false, doExonMetrics = false, exonic = false, legacyJunction = false, legacyNotExonic = false; //various booleans for keeping track of the alignment
        bool legacyNotSplit = false; //Legacy bug to override a read being split
        Strand read_strand = feature_strand(alignment, orientation);
        for (auto result = results->begin(); result != results->end(); ++result)
        {
            Feature exon;
            bool legacyFoundExon = false, legacyFoundGene = false, legacyTranscriptIntron = false, legacyTranscriptExon = false;
            map<string, float> legacySplitDosage;
            legacyNotSplit = false;
            if (result->type == FeatureType::Gene)
            {
                
                if (result->strand == Strand::Forward) transcriptPlus = true;
                else if (result->strand == Strand::Reverse) transcriptMinus = true;
                for (auto block = blocks.begin(); block != blocks.end(); ++block)
                {
                    if (read_strand != Strand::Unknown && read_strand != result->strand) continue;
                    intragenic = true;
                    
                    if (block->start > result->end) legacyNotExonic = true;
                    
                    bool firstexon = false;
                    legacyFoundExon = false;
                    
                    // No condition. just a scope to keep things clean
                    {
                        legacyFoundGene = true;
                        for (auto ex = results->begin(); ex != results->end() && !firstexon ; ++ex)
                        {
                            if (ex->type == FeatureType::Exon && ex->gene_id == result->gene_id && intersectInterval(*ex, *block)  )
                            {
                                if (result->ribosomal) ribosomal = true;
                                if (partialIntersect(*ex, *block) == (block->end - block->start))
                                {
                                    exon = *ex;
                                    legacyTranscriptExon = true;
                                    firstexon = true;
                                    legacyFoundExon=true; //should this be part of the loop condition?  look into overlapsIntronp
                                    baseCoverage.add(*ex, block->start, block->end);
                                }
                                else if (partialIntersect(*ex, *block) > 0)
                                {
                                    legacyTranscriptIntron = true;
                                }
                            }
                            
                        }
                        if (split && !legacyNotSplit)
                        {
                            if (legacyFoundExon)
                            {
                                legacySplitDosage[exon.feature_id] += (float) (block->end - block->start) / (float) alignment.Length();//length;
                            }
                            else legacyNotSplit = true;
                        }
                    }
                    
                    
                }
                //record gene to collect, probs want to keep a list of single and partial collections
                if (legacyFoundExon)
                {
                    if (highQuality)
                    {
                        if (split && !legacyNotSplit)
                        {
                            for (auto coverage = legacySplitDosage.begin(); coverage != legacySplitDosage.end(); ++coverage)
                            {
                                exonCounts[coverage->first] += coverage->second;
                                //                        cout << "\t" << coverage->first << " " << coverage->second;
                            }
                        }
                        else
                        {
                            //If read was not detected as split or the legacy bug changed it to unsplit, only record last exon
                            exonCounts[exon.feature_id] += 1.0;
                            //                    cout << "\t" << exon.feature_id<< " 1.0";
                        }
                        geneCounts[exon.gene_id] += 1.0;
                        if (fragmentTracker[exon.gene_id].count(alignment.Qname()) == 0)
                        {
                            fragmentTracker[exon.gene_id].insert(alignment.Qname());
                            geneFragmentCounts[exon.gene_id]++;
                        }
                        if (!alignment.DuplicateFlag()) uniqueGeneCounts[exon.gene_id]++;
                        baseCoverage.commit(exon.gene_id);
                    }
                    doExonMetrics = true;
                }
                if (legacyTranscriptIntron && legacyTranscriptExon) legacyJunction = true;
                if (legacyTranscriptExon) exonic = true;
            }
            
        }
        //    cout << endl;
        delete results; //clean up dynamic allocation
        
        if (legacyNotExonic || legacyJunction || !exonic) //a.k.a: No exons were detected at all on any block of the read
        {
            if (intragenic)
            {
                counter.increment("Intronic Reads");
                counter.increment("Intragenic Reads");
                if (highQuality){
                    counter.increment("HQ Intronic Reads");
                    counter.increment("HQ Intragenic Reads");
                }
            }
            else
            {
                counter.increment("Intergenic Reads");
                if (highQuality) counter.increment("HQ Intergenic Reads");
            }
        }
        else if (doExonMetrics && !legacyJunction && !legacyNotExonic) //if exons were detected and at least one exon ended up being collected, we count this as exonic
        {
            counter.increment("Exonic Reads");
            counter.increment("Intragenic Reads");
            if (highQuality)
            {
                counter.increment("HQ Exonic Reads");
                counter.increment("HQ Intragenic Reads");
            }
            if (split && !legacyNotSplit) counter.increment("Split Reads");
        }
        else if (intragenic)
        {
            //It's unclear how to properly classify these reads
            //However, the legacy tool falls back on reads being exonic
            counter.increment("Exonic Reads");
            counter.increment("Intragenic Reads");
            if (highQuality)
            {
                counter.increment("HQ Exonic Reads");
                counter.increment("HQ Intragenic Reads");
            }
        }
        if (ribosomal) counter.increment("rRNA Reads");
        //also record strandedness counts
        if ((transcriptMinus ^ transcriptPlus) && (singleEnd || alignment.PairedFlag()))
        {
            if (singleEnd || alignment.FirstFlag())
            {
                if (alignment.ReverseFlag()) transcriptMinus ? counter.increment("End 1 Sense") : counter.increment("End 1 Antisense");
                else transcriptPlus ? counter.increment("End 1 Sense") : counter.increment("End 1 Antisense");
            }
            else
            {
                if (alignment.ReverseFlag()) transcriptMinus ? counter.increment("End 2 Sense") : counter.increment("End 2 Antisense");
                else transcriptPlus ? counter.increment("End 2 Sense") : counter.increment("End 2 Antisense");
            }
        }
        baseCoverage.reset();
    }
    
    // New version of exon metrics
    // More efficient and less buggy
    double exonAlignmentMetrics(map<chrom, list<Feature>> &features, Metrics &counter, vector<Feature> &blocks, Alignment &alignment, SeqLib::HeaderSequenceVector &sequenceTable, unsigned int length, Strand orientation, BaseCoverage &baseCoverage, const bool highQuality, const bool singleEnd, map<string, FragmentMateEntry> &fragments, Fasta &fastaReader)
    {
        string chrName = sequenceTable[alignment.ChrID()].Name;
        chrom chr = chromosomeMap(chrName); //generate the chromosome shorthand name
        
        //Bamtools uses 0-based indexing because it's the 'norm' in computer science, even though bams are 1-based
        Feature current; //current block of the alignment (used while iterating)
        current.start = alignment.Position()+1; //0-based + 1 == 1-based
        current.end = alignment.PositionEnd(); //0-based, open == 1-based, closed
        
        vector<set<string> > genes; //each set is the set of genes intersected by the current block (one set per block)
        set<string> alignedExons; // Record of all aligned exons (make sure all blocks align to same exon for gc content)
        Collector exonCoverageCollector(&exonCounts); //Collects coverage counts for later (counts may be discarded)
        bool intragenic = false, transcriptPlus = false, transcriptMinus = false, ribosomal = false, doExonMetrics = false, exonic = false; //various booleans for keeping track of the alignment
        
        Strand read_strand = feature_strand(alignment, orientation);
        
        for (auto block = blocks.begin(); block != blocks.end(); ++block)
        {
            genes.push_back(set<string>()); //create a new set for this block
            list<Feature> *results = intersectBlock(*block, features[chr]); //grab the list of intersecting features
            for (auto result = results->begin(); result != results->end(); ++result)
            {
                if (read_strand != Strand::Unknown && read_strand != result->strand) continue;
                if (result->strand == Strand::Forward) transcriptPlus = true;
                else if (result->strand == Strand::Reverse) transcriptMinus = true;
                //else...what, exactly?
                if (result->type == FeatureType::Exon)
                {
                    exonic = true;
                    int intersectionSize = partialIntersect(*result, *block);
                    //check that this block fully overlaps the feature
                    //(if any bases of the block don't overlap, then the read is discarded)
                    if (intersectionSize == block->end - block->start)
                    {
                        //store the exon split dosage coverage in the collector for now
                        genes.rbegin()->insert(result->gene_id);
                        double tmp = static_cast<double>(intersectionSize) / length;
                        exonCoverageCollector.add(result->gene_id, result->feature_id, tmp);
                        baseCoverage.add(*result, block->start, block->end); //provisionally add per-base coverage to this gene
                        alignedExons.insert(result->feature_id);
                    }
                    
                }
                else if (result->type == FeatureType::Gene)
                {
                    intragenic = true;
                    //we don't record the gene name here because in terms of gene coverage and detection, we only care about exons
                    
                }
                if (result->ribosomal) ribosomal = true;
            }
            delete results; //clean up dynamic allocation
        }
        
        if (genes.size() >= 1)
        {
            //if there was more than one block, iterate through each block's set of genes and intersect them
            //In the end, we only care about genes that are common to each block
            //In theory, there's only one gene per block (in most cases) but I won't limit us on that assumption
            set<string> last = genes.front();
            for (int i = 1; i < genes.size(); ++i)
            {
                set<string> tmp;
                set_intersection(last.begin(), last.end(), genes[i].begin(), genes[i].end(), inserter(tmp, tmp.begin()));
                last = tmp;
            }
            //after the intersection, iterate over the remaining genes and record their coverage
            //at this point, "last" contains the set of genes which were unamgiguously aligned to
            for (auto gene = last.begin(); gene != last.end(); ++gene)
            {
                if (highQuality) {
                    if (exonCoverageCollector.queryGene(*gene))
                    {
                        geneCounts[*gene]++;
                        if (fragmentTracker[*gene].count(alignment.Qname()) == 0)
                        {
                            fragmentTracker[*gene].insert(alignment.Qname());
                            geneFragmentCounts[*gene]++;
                        }
                        if (!alignment.DuplicateFlag()) uniqueGeneCounts[*gene]++;
                    }
                    exonCoverageCollector.collect(*gene); //collect and keep exon coverage for this gene
                    baseCoverage.commit(*gene); //keep the per-base coverage recorded on this gene
                }
                doExonMetrics = true;
            }
            //check if this is a globin read
            set<string> globinIntersection, unambiguousGeneNames;
            for (const string& gene_id : last) unambiguousGeneNames.insert(geneNames[gene_id]); // translate set of gene_ids to set of gene names
            set_intersection(unambiguousGeneNames.begin(), unambiguousGeneNames.end(), blacklistedGlobins.begin(), blacklistedGlobins.end(), inserter(globinIntersection, globinIntersection.begin()));
            if (globinIntersection.empty())
            {
                // no unambiguous intersections with globins
                counter.increment("Non-Globin Reads");
                if (alignment.DuplicateFlag()) counter.increment("Non-Globin Duplicate Reads");
            }
        }
        
        if (!exonic) //a.k.a: No exons were detected at all on any block of the read
        {
            if (intragenic)
            {
                counter.increment("Intronic Reads");
                counter.increment("Intragenic Reads");
                if (highQuality){
                    counter.increment("HQ Intronic Reads");
                    counter.increment("HQ Intragenic Reads");
                }
            }
            else
            {
                counter.increment("Intergenic Reads");
                if (highQuality) counter.increment("HQ Intergenic Reads");
            }
        }
        else if (doExonMetrics) //if exons were detected and at least one exon ended up being collected, we count this as exonic
        {
            counter.increment("Exonic Reads");
            counter.increment("Intragenic Reads");
            if (highQuality)
            {
                counter.increment("HQ Exonic Reads");
                counter.increment("HQ Intragenic Reads");
            }
        }
        else
        {
            //It's unclear how to properly classify these reads
            //They had exon coverage, but aligned to multiple genes
            //Any exon and gene coverage they had was discarded and not recorded
            counter.increment("Ambiguous Reads");
            if (highQuality) counter.increment("HQ Ambiguous Reads");
        }
        if (ribosomal) counter.increment("rRNA Reads");
        //also record strandedness counts
        //TODO: check standing metrics.  Counts are probably off because of null intron/exon calls
        if ((transcriptMinus ^ transcriptPlus) && (singleEnd || alignment.PairedFlag()))
        {
            if (singleEnd || alignment.FirstFlag())
            {
                if (alignment.ReverseFlag()) transcriptMinus ? counter.increment("End 1 Sense") : counter.increment("End 1 Antisense");
                else transcriptPlus ? counter.increment("End 1 Sense") : counter.increment("End 1 Antisense");
            }
            else
            {
                if (alignment.ReverseFlag()) transcriptMinus ? counter.increment("End 2 Sense") : counter.increment("End 2 Antisense");
                else transcriptPlus ? counter.increment("End 2 Sense") : counter.increment("End 2 Antisense");
            }
        }
        baseCoverage.reset();
        if (fastaReader.hasContig(chr) && highQuality && exonic && doExonMetrics && alignedExons.size() == 1 && blocks.size() == 1 && fabs(alignment.InsertSize()) > 100 && fabs(alignment.InsertSize()) < 1000) {
            string exonName = *(alignedExons.begin());
            auto fragment = fragments.find(alignment.Qname());
            if (fragment == fragments.end()) //first time we've encountered a read in this pair
            {
                // Record the exon we aligned to and the actual end of the read
                fragments[alignment.Qname()] = std::make_tuple(exonName, alignment.PositionEnd());
            }
            else if (exonName == std::get<EXON>(fragment->second)) //second time we've encountered a read in this pair
            {
                
                //Check that we end after the mate ends, and that we aren't aligned to the same start position
                if (alignment.PositionEnd() <= std::get<ENDPOS>(fragment->second) || alignment.Position() == alignment.MatePosition()) return -1;
                //This pair is useable for fragment statistics:  both pairs fully aligned to the same exon
                string seq = fastaReader.getSeq(chr, std::get<ENDPOS>(fragment->second) - alignment.Length(), alignment.PositionEnd());
                fragments.erase(alignment.Qname());
                return seq.length() > 0 ? gc(seq) : -1;
            }
        }
        return -1;
    }

    // Estimate fragment size in a read pair
    void fragmentSizeMetrics(unsigned int &doFragmentSize, map<chrom, list<Feature>> *bedFeatures, map<string, FragmentMateEntry> &fragments, map<long long, unsigned long> &fragmentSizes, vector<Feature> &blocks, Alignment &alignment, SeqLib::HeaderSequenceVector &sequenceTable)
    {
        string chrName = sequenceTable[alignment.ChrID()].Name;
        chrom chr = chromosomeMap(chrName); //generate the chromosome shorthand referemce
        bool firstBlock = true, sameExon = true; //for keeping track of the alignment state
        string exonName = ""; // the name of the intersected exon from the bed
        
        trimFeatures(alignment, (*bedFeatures)[chr]); //trim out the features to speed up intersections
        for (auto block = blocks.begin(); sameExon && block != blocks.end(); ++block)
        {
            //for each block, intersect it with the bed file features
            list<Feature> *results = intersectBlock(*block, (*bedFeatures)[chr]);
            if (results->size() == 1 && (partialIntersect(results->front(), *block) == (block->end - block->start))) //if the block intersected more than one exon, it's immediately disqualified
            {
                if (firstBlock) exonName = results->begin()->feature_id; //record the exon name on the first pass
                else if (exonName != results->begin()->feature_id) //ensure the same exon name on subsequent passes
                {
                    sameExon = false;
                    delete results;
                    break;
                }
            }
            else sameExon = false;
            delete results; //clean up dynamic allocation
            firstBlock = false;
        }
        if (sameExon && exonName.size()) //if all blocks intersected the same exon, take a fragment size sample
        {
            //both mates in a pair have to intersected the same exon in order for the pair to qualify for the sample
            auto fragment = fragments.find(alignment.Qname());
            if (fragment == fragments.end()) //first time we've encountered a read in this pair
            {
                // Record the exon we aligned to and the actual end of the read
                fragments[alignment.Qname()] = std::make_tuple(exonName, alignment.PositionEnd());
            }
            else if (exonName == std::get<EXON>(fragment->second)) //second time we've encountered a read in this pair
            {
                //Quick test: Does the mate startP occur inside the aligned range of this read?
//                if (alignment.PositionEndMate() >= alignment.Position() && alignment.PositionEndMate() <= alignment.PositionEnd()) return doFragmentSize;
                // Check 4 conditions:
                // 1) This read must always be reverse. If the + strand read occurs AFTER the - read, there has been a mapping error or genomic translocation
                // 2) The mate must always be +. If both reads are on the - strand, there has been a mapping error or genomic inversion
                // 3) The this read ends after the mate does. If this read is contained by the mate, there has been some weird clipping errors with the cDNA adapters
                // 4) This read must not start at the same point as the mate. If so, without this check, the pair may be arbitrarily discarded or kept depending on sort order
                
                //FIXME: Is the above test actually accurate? Cant a + read appear after a - read for reverse strand alignments?
                if (alignment.MateReverseFlag() || !alignment.ReverseFlag() || alignment.PositionEnd() <= std::get<ENDPOS>(fragment->second)  || alignment.Position() == alignment.MatePosition()) return;
                //This pair is useable for fragment statistics:  both pairs fully aligned to the same exon
                fragmentSizes[abs(alignment.InsertSize())] += 1;
                fragments.erase(fragment);
                --doFragmentSize;
                if (!doFragmentSize)
                {
                    delete bedFeatures; //after taking all the samples we need, clean up the dynamic allocation
                    bedFeatures = nullptr;
                }
            }
        }
    }


    /*double gcContent(unsigned int &doFragmentSize, map<chrom, list<Feature>> *bedFeatures, map<string, FragmentMateEntry> &fragments, map<long long, unsigned long> &fragmentSizes, vector<Feature> &blocks, Alignment &alignment, SeqLib::HeaderSequenceVector &sequenceTable, Fasta &fastaReader)
    {
        string chrName = sequenceTable[alignment.ChrID()].Name;
        chrom chr = chromosomeMap(chrName); //generate the chromosome shorthand referemce
        bool firstBlock = true, sameExon = true; //for keeping track of the alignment state
        string exonName = ""; // the name of the intersected exon from the bed
        
        trimFeatures(alignment, (*bedFeatures)[chr]); //trim out the features to speed up intersections
        for (auto block = blocks.begin(); sameExon && block != blocks.end(); ++block)
        {
            //for each block, intersect it with the bed file features
            list<Feature> *results = intersectBlock(*block, (*bedFeatures)[chr]);
            if (results->size() == 1 && (partialIntersect(results->front(), *block) == (block->end - block->start))) //if the block intersected more than one exon, it's immediately disqualified
            {
                if (firstBlock) exonName = results->begin()->feature_id; //record the exon name on the first pass
                else if (exonName != results->begin()->feature_id) //ensure the same exon name on subsequent passes
                {
                    sameExon = false;
                    delete results;
                    break;
                }
            }
            else sameExon = false;
            delete results; //clean up dynamic allocation
            firstBlock = false;
        }
        if (sameExon && exonName.size()) //if all blocks intersected the same exon, take a fragment size sample
        {
            //both mates in a pair have to intersected the same exon in order for the pair to qualify for the sample
            
        }
        //return the remaining count of fragment samples to take
        return -1;
    }*/
}
