//
// <copyright file="BlockRandomizer.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
// BlockRandomizer.h -- interface of the block randomizer
//

#pragma once

#include "Basics.h"                  // for attempt()
#include "htkfeatio.h"                  // for htkmlfreader
#include "latticearchive.h"             // for reading HTK phoneme lattices (MMI training)
#include "minibatchsourcehelpers.h"
#include "minibatchiterator.h"
#include "biggrowablevectors.h"
#include "ssematrix.h"
#include "unordered_set"
#include "inner_interfaces.h"

namespace msra { namespace dbn {

    // TODO the following should move
    // data store (incl. paging in/out of features and lattices)
    struct utterancedesc            // data descriptor for one utterance
    {
        msra::asr::htkfeatreader::parsedpath parsedpath;    // archive filename and frame range in that file
        size_t classidsbegin;       // index into allclassids[] array (first frame)

        utterancedesc (msra::asr::htkfeatreader::parsedpath&& ppath, size_t classidsbegin) : parsedpath (std::move(ppath)), classidsbegin (classidsbegin) {}

        const wstring & logicalpath() const { return parsedpath; /*type cast will return logical path*/ }
        size_t numframes() const { return parsedpath.numframes(); }
        wstring key() const                           // key used for looking up lattice (not stored to save space)
        {
#ifdef _MSC_VER
            static const wstring emptywstring;
            static const wregex deleteextensionre (L"\\.[^\\.\\\\/:]*$");
            return regex_replace (logicalpath(), deleteextensionre, emptywstring);  // delete extension (or not if none)
#else
            return removeExtension(logicalpath());
#endif
        }
    };

    // Make sure type 'utterancedesc' has a move constructor
    static_assert(std::is_move_constructible<utterancedesc>::value, "Type 'utterancedesc' should be move constructible!");

    struct utterancechunkdata       // data for a chunk of utterances
    {
        std::vector<utterancedesc> utteranceset;    // utterances in this set
        size_t numutterances() const { return utteranceset.size(); }

        std::vector<size_t> firstframes;    // [utteranceindex] first frame for given utterance
        mutable msra::dbn::matrix frames;   // stores all frames consecutively (mutable since this is a cache)
        size_t totalframes;         // total #frames for all utterances in this chunk
        mutable std::vector<shared_ptr<const latticesource::latticepair>> lattices;   // (may be empty if none)

        // construction
        utterancechunkdata() : totalframes (0) {}
        void push_back (utterancedesc &&/*destructive*/ utt)
        {
            if (isinram())
                LogicError("utterancechunkdata: frames already paged into RAM--too late to add data");
            firstframes.push_back (totalframes);
            totalframes += utt.numframes();
            utteranceset.push_back (std::move(utt));
        }

        // accessors to an utterance's data
        size_t numframes (size_t i) const { return utteranceset[i].numframes(); }
        size_t getclassidsbegin (size_t i) const { return utteranceset[i].classidsbegin; }
        msra::dbn::matrixstripe getutteranceframes (size_t i) const // return the frame set for a given utterance
        {
            if (!isinram())
                LogicError("getutteranceframes: called when data have not been paged in");
            const size_t ts = firstframes[i];
            const size_t n = numframes(i);
            return msra::dbn::matrixstripe (frames, ts, n);
        }
        shared_ptr<const latticesource::latticepair> getutterancelattice (size_t i) const // return the frame set for a given utterance
        {
            if (!isinram())
                LogicError("getutterancelattice: called when data have not been paged in");
            return lattices[i];
        }

        // paging
        // test if data is in memory at the moment
        bool isinram() const { return !frames.empty(); }
        // page in data for this chunk
        // We pass in the feature info variables by ref which will be filled lazily upon first read
        void requiredata (string & featkind, size_t & featdim, unsigned int & sampperiod, const latticesource & latticesource, int verbosity=0) const
        {
            if (numutterances() == 0)
                LogicError("requiredata: cannot page in virgin block");
            if (isinram())
                LogicError("requiredata: called when data is already in memory");
            try             // this function supports retrying since we read from the unrealible network, i.e. do not return in a broken state
            {
                msra::asr::htkfeatreader reader;    // feature reader (we reinstantiate it for each block, i.e. we reopen the file actually)
                // if this is the first feature read ever, we explicitly open the first file to get the information such as feature dimension
                if (featdim == 0)
                {
                    reader.getinfo (utteranceset[0].parsedpath, featkind, featdim, sampperiod);
                    fprintf(stderr, "requiredata: determined feature kind as %llu-dimensional '%s' with frame shift %.1f ms\n",
                        featdim, featkind.c_str(), sampperiod / 1e4);
                }
                // read all utterances; if they are in the same archive, htkfeatreader will be efficient in not closing the file
                frames.resize (featdim, totalframes);
                if (!latticesource.empty())
                    lattices.resize (utteranceset.size());
                foreach_index (i, utteranceset)
                {
                    //fprintf (stderr, ".");
                    // read features for this file
                    auto uttframes = getutteranceframes (i);    // matrix stripe for this utterance (currently unfilled)
                    reader.read (utteranceset[i].parsedpath, (const string &) featkind, sampperiod, uttframes);  // note: file info here used for checkuing only
                    // page in lattice data
                    if (!latticesource.empty())
                        latticesource.getlattices (utteranceset[i].key(), lattices[i], uttframes.cols());
                }
                //fprintf (stderr, "\n");
                if (verbosity)
                    fprintf (stderr, "requiredata: %d utterances read\n", (int)utteranceset.size());
            }
            catch (...)
            {
                releasedata();
                throw;
            }
        }
        // page out data for this chunk
        void releasedata() const
        {
            if (numutterances() == 0)
                LogicError("releasedata: cannot page out virgin block");
            if (!isinram())
                LogicError("releasedata: called when data is not memory");
            // release frames
            frames.resize (0, 0);
            // release lattice data
            lattices.clear();
        }
    };

    class BlockRandomizer : public Microsoft::MSR::CNTK::Transformer
    {
        int m_verbosity;
        bool m_framemode;
        size_t m_totalframes;
        size_t m_numutterances;
        size_t m_randomizationrange; // parameter remembered; this is the full window (e.g. 48 hours), not the half window
        size_t m_currentSweep;            // randomization is currently cached for this sweep; if it changes, rebuild all below
        Microsoft::MSR::CNTK::SequencerPtr m_sequencer;
        size_t m_currentSequenceId; // position within the current sweep
        Microsoft::MSR::CNTK::Timeline m_randomTimeline;

        // TODO note: numutterances / numframes could also be computed through neighbors
        struct chunk                    // chunk as used in actual processing order (randomized sequence)
        {
            size_t originalChunkIndex;
            size_t numutterances;
            size_t numframes;

            // position in utterance-position space
            size_t utteranceposbegin;
            size_t utteranceposend() const { return utteranceposbegin + numutterances; }

            // TODO ts instead of globalts, merge with pos ?

            // position on global time line
            size_t globalts;            // start frame on global timeline (after randomization)
            size_t globalte() const { return globalts + numframes; }

            // randomization range limits
            size_t windowbegin;         // randomizedchunk index of earliest chunk that utterances in here can be randomized with
            size_t windowend;           // and end index [windowbegin, windowend)
            chunk(size_t originalChunkIndex,
                size_t numutterances,
                size_t numframes,
                size_t utteranceposbegin,
                size_t globalts)
                : originalChunkIndex(originalChunkIndex)
                , numutterances(numutterances)
                , numframes(numframes)
                , utteranceposbegin(utteranceposbegin)
                , globalts(globalts) {}
        };
        std::vector<chunk> randomizedchunks;  // utterance chunks after being brought into random order (we randomize within a rolling window over them)

    private:
        // TODO rename
        std::unordered_map<size_t, size_t> randomizedutteranceposmap;     // [globalts] -> pos lookup table // TODO not valid for new randomizer

        struct positionchunkwindow       // chunk window required in memory when at a certain position, for controlling paging
        {
            std::vector<chunk>::iterator definingchunk;       // the chunk in randomizedchunks[] that defined the utterance position of this utterance
            size_t windowbegin() const { return definingchunk->windowbegin; }
            size_t windowend() const { return definingchunk->windowend; }

            bool isvalidforthisposition (const Microsoft::MSR::CNTK::SequenceDescription & sequence) const
            {
                return sequence.chunkId >= windowbegin() && sequence.chunkId < windowend(); // check if 'sequence' lives in is in allowed range for this position
                // TODO by construction sequences cannot span chunks (check again)
            }

            positionchunkwindow (std::vector<chunk>::iterator definingchunk) : definingchunk (definingchunk) {}
        };
        std::vector<positionchunkwindow> positionchunkwindows;      // [utterance position] -> [windowbegin, windowend) for controlling paging
        // TODO improve, use randomized timeline?

        template<typename VECTOR> static void randomshuffle (VECTOR & v, size_t randomseed);

    public:
        BlockRandomizer(int verbosity, bool framemode, size_t totalframes, size_t numutterances, size_t randomizationrange, Microsoft::MSR::CNTK::SequencerPtr sequencer)
            : m_verbosity(verbosity)
            , m_framemode(framemode)
            , m_totalframes(totalframes)
            , m_numutterances(numutterances)
            , m_randomizationrange(randomizationrange)
            , m_currentSweep(SIZE_MAX)
            , m_currentSequenceId(SIZE_MAX)
            , m_sequencer(sequencer)
        {
            assert(sequencer != nullptr); // TODO only new mode
        }

        void LazyRandomize();

        void Randomize(
            const size_t sweep,
            const size_t sweepts,
            const Microsoft::MSR::CNTK::Timeline& timeline);

        size_t getSequenceWindowBegin(size_t sequenceIndex) const
        {
            assert(sequenceIndex < positionchunkwindows.size());
            return positionchunkwindows[sequenceIndex].windowbegin();
        }

        size_t getSequenceWindowEnd(size_t sequenceIndex) const
        {
            assert(sequenceIndex < positionchunkwindows.size());
            return positionchunkwindows[sequenceIndex].windowend();
        }

        virtual void SetEpochConfiguration(const Microsoft::MSR::CNTK::EpochConfiguration& config) override;

        virtual std::vector<Microsoft::MSR::CNTK::InputDescriptionPtr> getInputs() const override
        {
            std::vector<Microsoft::MSR::CNTK::InputDescriptionPtr> dummy;
            return dummy;
        }

        virtual ~BlockRandomizer()
        {
        }

        virtual Microsoft::MSR::CNTK::SequenceData getNextSequence() override;

    private:
        bool IsValid(const Microsoft::MSR::CNTK::Timeline& timeline) const;

        Microsoft::MSR::CNTK::EpochConfiguration m_config;
        size_t m_currentFrame;
        size_t m_epochSize;
    };
} }