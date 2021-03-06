/*
 * Copyright 2013-2019 Stefan Widgren and Maria Noremark,
 * National Veterinary Institute, Sweden
 *
 * Licensed under the EUPL, Version 1.1 or - as soon they
 * will be approved by the European Commission - subsequent
 * versions of the EUPL (the "Licence");
 * You may not use this work except in compliance with the
 * Licence.
 * You may obtain a copy of the Licence at:
 *
 * http: *ec.europa.eu/idabc/eupl
 *
 * Unless required by applicable law or agreed to in
 * writing, software distributed under the Licence is
 * distributed on an "AS IS" basis,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the Licence for the specific language governing
 * permissions and limitations under the Licence.
 */

#define R_NO_REMAP
#define STRICT_R_HEADERS

#include "kvec.h"
#include <string.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <Rinternals.h>
#include <R_ext/Rdynload.h>
#include <R_ext/Visibility.h>

typedef struct Contact
{
  int rowid;
  int identifier;
  int t;
} Contact;

class CompareContact {
public:
    bool operator()(const Contact& c, int t) {
        return c.t < t;
    }

    bool operator()(int t, const Contact& c) {
        return t < c.t;
    }
};

/* Help class to keep track of visited nodes. */
class VisitedNodes {
public:
    VisitedNodes(size_t numberOfIdentifiers)
        : numberOfVisitedNodes(0),
          visitedNodes(numberOfIdentifiers)
        {}

    int N(void) const {
        return numberOfVisitedNodes;
    }

    void Update(int node, int tBegin, int tEnd, bool ingoing) {
        if (visitedNodes[node].first) {
            if (ingoing) {
                if (tEnd > visitedNodes[node].second) {
                    visitedNodes[node].second = tEnd;
                }
            } else if (tBegin < visitedNodes[node].second) {
                visitedNodes[node].second = tBegin;
            }
        } else {
            visitedNodes[node].first = true;
            numberOfVisitedNodes++;
            if (ingoing)
                visitedNodes[node].second = tEnd;
            else
                visitedNodes[node].second = tBegin;
        }
    }

    bool Visit(int node, int tBegin, int tEnd, bool ingoing) {
        if (visitedNodes[node].first) {
            if (ingoing) {
                if (tEnd <= visitedNodes[node].second) {
                    return false;
                }
            } else if (tBegin >= visitedNodes[node].second) {
                return false;
            }
        }

        return true;
    }

private:
    int numberOfVisitedNodes;
    std::vector<std::pair<bool, int> > visitedNodes;
};

typedef std::vector<Contact> Contacts;

static int check_arguments(
    SEXP src,
    SEXP dst,
    SEXP t,
    SEXP root,
    SEXP inBegin,
    SEXP inEnd,
    SEXP outBegin,
    SEXP outEnd,
    SEXP numberOfIdentifiers)
{
    if (Rf_isNull(root) ||
        Rf_isNull(inBegin) ||
        Rf_isNull(inEnd) ||
        Rf_isNull(outBegin) ||
        Rf_isNull(outEnd) ||
        Rf_isNull(numberOfIdentifiers) ||
        !Rf_isInteger(root) ||
        !Rf_isInteger(inBegin) ||
        !Rf_isInteger(inEnd) ||
        !Rf_isInteger(outBegin) ||
        !Rf_isInteger(outEnd) ||
        !Rf_isInteger(numberOfIdentifiers) ||
        Rf_xlength(numberOfIdentifiers) != 1)
        return 1;
    return 0;
}

static int buildContactsLookup(
    std::vector<std::map<int, Contacts> >& ingoing,
    std::vector<std::map<int, Contacts> >& outgoing,
    SEXP src,
    SEXP dst,
    SEXP t)
{
    int *ptr_src = INTEGER(src);
    int *ptr_dst = INTEGER(dst);
    int *ptr_t = INTEGER(t);
    R_xlen_t len = Rf_xlength(t);
    int *rowid = (int *)malloc(len * sizeof(int));
    if (!rowid)
        return -1;

    /* The contacts must be sorted by t. */
    R_orderVector(rowid, len, Rf_lang1(t), FALSE, FALSE);

    for (R_xlen_t i = 0; i < len; ++i) {
        int j = rowid[i];

        /* Decrement with one since C is zero-based. */
        int zb_src = ptr_src[j] - 1;
        int zb_dst = ptr_dst[j] - 1;

        ingoing[zb_dst][zb_src].push_back((Contact){j, zb_src, ptr_t[j]});
        outgoing[zb_src][zb_dst].push_back((Contact){j, zb_dst, ptr_t[j]});
    }

    free(rowid);

    return 0;
}

static void
doShortestPaths(const std::vector<std::map<int, Contacts> >& data,
                const int node,
                const int tBegin,
                const int tEnd,
                std::set<int> visitedNodes,
                const int distance,
                const bool ingoing,
                std::map<int, std::pair<int, int> >& result)
{
    visitedNodes.insert(node);

    for (std::map<int, Contacts>::const_iterator it = data[node].begin(),
            end = data[node].end(); it != end; ++it)
    {
        /* We are not interested in going in loops or backwards in the
         * search path. */
        if (visitedNodes.find(it->first) == visitedNodes.end()) {
            /* We are only interested in contacts within the specified
             * time period, so first check the lower bound, tBegin. */
            Contacts::const_iterator t_begin =
                std::lower_bound(it->second.begin(),
                                 it->second.end(),
                                 tBegin,
                                 CompareContact());

            if (t_begin != it->second.end() && t_begin->t <= tEnd) {
                int t0, t1;

                std::map<int, std::pair<int, int> >::iterator distance_it =
                    result.find(it->first);
                if (distance_it == result.end()) {
                    result[it->first].first = distance;

                    /* Increment with one since R vector is
                     * one-based. */
                    result[it->first].second = t_begin->rowid + 1;
                }  else if (distance < distance_it->second.first) {
                    distance_it->second.first = distance;

                    /* Increment with one since R vector is
                     * one-based. */
                    distance_it->second.second = t_begin->rowid + 1;
                }

                if (ingoing) {
                    /* and then the upper bound, tEnd. */
                    Contacts::const_iterator t_end =
                        std::upper_bound(t_begin,
                                         it->second.end(),
                                         tEnd,
                                         CompareContact());

                    t0 = tBegin;
                    t1 = (t_end-1)->t;
                } else {
                    t0 = t_begin->t;
                    t1 = tEnd;
                }

                doShortestPaths(data,
                                it->first,
                                t0,
                                t1,
                                visitedNodes,
                                distance + 1,
                                ingoing,
                                result);
            }
        }
    }
}

extern "C" SEXP shortestPaths(
    SEXP src,
    SEXP dst,
    SEXP t,
    SEXP root,
    SEXP inBegin,
    SEXP inEnd,
    SEXP outBegin,
    SEXP outEnd,
    SEXP numberOfIdentifiers)
{
    const char *names[] = {"inDistance", "inRowid", "inIndex",
                           "outDistance", "outRowid", "outIndex", ""};
    kvec_t(int) inRowid;
    kvec_t(int) outRowid;
    kvec_t(int) inDistance;
    kvec_t(int) outDistance;
    kvec_t(int) inIndex;
    kvec_t(int) outIndex;
    SEXP result, vec;
    /* Lookup for ingoing contacts. */
    std::vector<std::map<int, Contacts> > ingoing(Rf_asInteger(numberOfIdentifiers));

    /* Lookup for outfoing contacts. */
    std::vector<std::map<int, Contacts> > outgoing(Rf_asInteger(numberOfIdentifiers));

    if (check_arguments(src, dst, t, root, inBegin, inEnd,
                       outBegin, outEnd, numberOfIdentifiers))
        Rf_error("Unable to calculate shortest paths");

    buildContactsLookup(ingoing, outgoing, src, dst, t);

    R_xlen_t len = Rf_xlength(root);
    kv_init(inRowid);
    kv_init(outRowid);
    kv_init(inDistance);
    kv_init(outDistance);
    kv_init(inIndex);
    kv_init(outIndex);

    for (R_xlen_t i = 0; i < len; ++i) {
        /* Key: node, Value: first: distance, second: original
         * rowid. */
        std::map<int, std::pair<int, int> > ingoingShortestPaths;

        /* Key: node, Value: first: distance, second: original
         * rowid. */
        std::map<int, std::pair<int, int> > outgoingShortestPaths;

        doShortestPaths(ingoing,
                        INTEGER(root)[i] - 1,
                        INTEGER(inBegin)[i],
                        INTEGER(inEnd)[i],
                        std::set<int>(),
                        1,
                        true,
                        ingoingShortestPaths);

        for (std::map<int, std::pair<int, int> >::const_iterator it =
                ingoingShortestPaths.begin();
            it!=ingoingShortestPaths.end(); ++it)
        {
            kv_push(int, inDistance, it->second.first);
            kv_push(int, inRowid, it->second.second);
            kv_push(int, inIndex, i + 1);
        }

        doShortestPaths(outgoing,
                        INTEGER(root)[i] - 1,
                        INTEGER(outBegin)[i],
                        INTEGER(outEnd)[i],
                        std::set<int>(),
                        1,
                        false,
                        outgoingShortestPaths);

        for (std::map<int, std::pair<int, int> >::const_iterator it =
                 outgoingShortestPaths.begin();
            it!=outgoingShortestPaths.end(); ++it)
        {
            kv_push(int, outDistance, it->second.first);
            kv_push(int, outRowid, it->second.second);
            kv_push(int, outIndex, i + 1);
        }
    }

    PROTECT(result = Rf_mkNamed(VECSXP, names));

    SET_VECTOR_ELT(result, 0, vec = Rf_allocVector(INTSXP, kv_size(inDistance)));
    memcpy(INTEGER(vec), &kv_A(inDistance, 0), kv_size(inDistance) * sizeof(int));

    SET_VECTOR_ELT(result, 1, vec = Rf_allocVector(INTSXP, kv_size(inRowid)));
    memcpy(INTEGER(vec), &kv_A(inRowid, 0), kv_size(inRowid) * sizeof(int));

    SET_VECTOR_ELT(result, 2, vec = Rf_allocVector(INTSXP, kv_size(inIndex)));
    memcpy(INTEGER(vec), &kv_A(inIndex, 0), kv_size(inIndex) * sizeof(int));

    SET_VECTOR_ELT(result, 3, vec = Rf_allocVector(INTSXP, kv_size(outDistance)));
    memcpy(INTEGER(vec), &kv_A(outDistance, 0), kv_size(outDistance) * sizeof(int));

    SET_VECTOR_ELT(result, 4, vec = Rf_allocVector(INTSXP, kv_size(outRowid)));
    memcpy(INTEGER(vec), &kv_A(outRowid, 0), kv_size(outRowid) * sizeof(int));

    SET_VECTOR_ELT(result, 5, vec = Rf_allocVector(INTSXP, kv_size(outIndex)));
    memcpy(INTEGER(vec), &kv_A(outIndex, 0), kv_size(outIndex) * sizeof(int));

cleanup:
    kv_destroy(inRowid);
    kv_destroy(outRowid);
    kv_destroy(inDistance);
    kv_destroy(outDistance);
    kv_destroy(inIndex);
    kv_destroy(outIndex);

    UNPROTECT(1);

    return result;
}

static void
doTraceContacts(const std::vector<std::map<int, Contacts> >& data,
                const int node,
                const int tBegin,
                const int tEnd,
                std::set<int> visitedNodes,
                const int distance,
                const bool ingoing,
                std::vector<int>& resultRowid,
                std::vector<int>& resultDistance,
                const int maxDistance)
{
    visitedNodes.insert(node);

    for (std::map<int, Contacts>::const_iterator it = data[node].begin(),
            end = data[node].end(); it != end; ++it)
    {
        /* We are not interested in going in loops or backwards in the
         * search path. */
        if (visitedNodes.find(it->first) == visitedNodes.end()) {
            /* We are only interested in contacts within the specified
             * time period, so first check the lower bound, tBegin. */
            Contacts::const_iterator t_begin =
                std::lower_bound(it->second.begin(),
                                 it->second.end(),
                                 tBegin,
                                 CompareContact());

            if (t_begin != it->second.end() && t_begin->t <= tEnd) {
                int t0, t1;

                /* and then the upper bound, tEnd. */
                Contacts::const_iterator t_end =
                    std::upper_bound(t_begin,
                                     it->second.end(),
                                     tEnd,
                                     CompareContact());

                for (Contacts::const_iterator iit=t_begin; iit!=t_end; ++iit) {
                    /* Increment with one since R vector is
                     * one-based. */
                    resultRowid.push_back(iit->rowid + 1);

                    resultDistance.push_back(distance);
                }

                if (maxDistance > 0 && distance >= maxDistance)
                    continue;

                if (ingoing) {
                    t0 = tBegin;
                    t1 = (t_end-1)->t;
                } else {
                    t0 = t_begin->t;
                    t1 = tEnd;
                }

                doTraceContacts(data,
                                it->first,
                                t0,
                                t1,
                                visitedNodes,
                                distance + 1,
                                ingoing,
                                resultRowid,
                                resultDistance,
                                maxDistance);
            }
        }
    }
}

extern "C" SEXP traceContacts(
    SEXP src,
    SEXP dst,
    SEXP t,
    SEXP root,
    SEXP inBegin,
    SEXP inEnd,
    SEXP outBegin,
    SEXP outEnd,
    SEXP numberOfIdentifiers,
    SEXP maxDistance)
{
    /* Lookup for ingoing contacts. */
    std::vector<std::map<int, Contacts> > ingoing(Rf_asInteger(numberOfIdentifiers));

    /* Lookup for outfoing contacts. */
    std::vector<std::map<int, Contacts> > outgoing(Rf_asInteger(numberOfIdentifiers));

    if (check_arguments(src, dst, t, root, inBegin, inEnd, outBegin, outEnd,
                        numberOfIdentifiers)) {
        Rf_error("Unable to trace contacts");
    }

    buildContactsLookup(ingoing, outgoing, src, dst, t);

    SEXP result, vec;
    std::vector<int> resultRowid;
    std::vector<int> resultDistance;

    PROTECT(result = Rf_allocVector(VECSXP, 4 * Rf_xlength(root)));
    for (R_xlen_t i = 0, end = Rf_xlength(root); i < end; ++i) {
        resultRowid.clear();
        resultDistance.clear();

        doTraceContacts(ingoing,
                        INTEGER(root)[i] - 1,
                        INTEGER(inBegin)[i],
                        INTEGER(inEnd)[i],
                        std::set<int>(),
                        1,
                        true,
                        resultRowid,
                        resultDistance,
                        INTEGER(maxDistance)[0]);

        SET_VECTOR_ELT(result, 4 * i, vec = Rf_allocVector(INTSXP, resultRowid.size()));
        for (size_t j = 0; j < resultRowid.size(); ++j)
            INTEGER(vec)[j] = resultRowid[j];

        SET_VECTOR_ELT(result, 4 * i + 1, vec = Rf_allocVector(INTSXP, resultDistance.size()));
        for (size_t j = 0; j < resultDistance.size(); ++j)
            INTEGER(vec)[j] = resultDistance[j];

        resultRowid.clear();
        resultDistance.clear();

        doTraceContacts(outgoing,
                        INTEGER(root)[i] - 1,
                        INTEGER(outBegin)[i],
                        INTEGER(outEnd)[i],
                        std::set<int>(),
                        1,
                        false,
                        resultRowid,
                        resultDistance,
                        INTEGER(maxDistance)[0]);

        SET_VECTOR_ELT(result, 4 * i + 2, vec = Rf_allocVector(INTSXP, resultRowid.size()));
        for (size_t j = 0; j < resultRowid.size(); ++j)
            INTEGER(vec)[j] = resultRowid[j];

        SET_VECTOR_ELT(result, 4 * i + 3, vec = Rf_allocVector(INTSXP, resultDistance.size()));
        for (size_t j = 0; j < resultDistance.size(); ++j)
            INTEGER(vec)[j] = resultDistance[j];
    }

cleanup:
    UNPROTECT(1);

    return result;
}

static int
degree(const std::vector<std::map<int, Contacts> >& data,
       const int node,
       const int tBegin,
       const int tEnd)
{
    int result = 0;

    for (std::map<int, Contacts>::const_iterator it = data[node].begin();
        it != data[node].end();
        ++it)
    {
        /* We are not interested in going in loops. */
        if (node != it->first) {
            /* We are only interested in contacts within the specified
             * time period, so first check the lower bound, tBegin. */
            Contacts::const_iterator t_begin =
                std::lower_bound(it->second.begin(),
                                 it->second.end(),
                                 tBegin,
                                 CompareContact());

            if (t_begin != it->second.end() && t_begin->t <= tEnd) {
                ++result;
            }
        }
    }

    return result;
}

static void
contactChain(const std::vector<std::map<int, Contacts> >& data,
	     const int node,
	     const int tBegin,
	     const int tEnd,
	     VisitedNodes& visitedNodes,
	     const bool ingoing)
{
    visitedNodes.Update(node, tBegin, tEnd, ingoing);

    for (std::map<int, Contacts>::const_iterator it = data[node].begin(),
            end = data[node].end(); it != end; ++it)
    {
        if (visitedNodes.Visit(it->first, tBegin, tEnd, ingoing)) {
            /* We are only interested in contacts within the specified
             * time period, so first check the lower bound, tBegin. */
            Contacts::const_iterator t_begin =
                std::lower_bound(it->second.begin(),
                                 it->second.end(),
                                 tBegin,
                                 CompareContact());

            if (t_begin != it->second.end() && t_begin->t <= tEnd) {
                int t0, t1;

                if (ingoing) {
                    /* and then the upper bound, tEnd. */
                    Contacts::const_iterator t_end =
                        std::upper_bound(t_begin,
                                         it->second.end(),
                                         tEnd,
                                         CompareContact());

                    t0 = tBegin;
                    t1 = (t_end-1)->t;
                } else {
                    t0 = t_begin->t;
                    t1 = tEnd;
                }

                contactChain(data, it->first, t0, t1, visitedNodes, ingoing);
            }
        }
    }
}

extern "C" SEXP networkSummary(
    SEXP src,
    SEXP dst,
    SEXP t,
    SEXP root,
    SEXP inBegin,
    SEXP inEnd,
    SEXP outBegin,
    SEXP outEnd,
    SEXP numberOfIdentifiers)
{
    const char *names[] = {"inDegree", "outDegree",
                           "ingoingContactChain", "outgoingContactChain", ""};
    int error = 0, nprotect = 0;
    kvec_t(int) ingoingContactChain;
    kvec_t(int) outgoingContactChain;
    kvec_t(int) inDegree;
    kvec_t(int) outDegree;
    SEXP result, vec;

    /* Lookup for ingoing contacts. */
    std::vector<std::map<int, Contacts> > ingoing(Rf_asInteger(numberOfIdentifiers));

    /* Lookup for outfoing contacts. */
    std::vector<std::map<int, Contacts> > outgoing(Rf_asInteger(numberOfIdentifiers));

    error = check_arguments(src, dst, t, root, inBegin, inEnd,
                            outBegin, outEnd, numberOfIdentifiers);
    if (error)
        goto cleanup;

    kv_init(ingoingContactChain);
    kv_init(outgoingContactChain);
    kv_init(inDegree);
    kv_init(outDegree);

    error = buildContactsLookup(ingoing, outgoing, src, dst, t);
    if (error)
        goto cleanup;

    for (R_xlen_t i = 0, end = Rf_xlength(root); i < end; ++i) {
        VisitedNodes visitedNodesIngoing(INTEGER(numberOfIdentifiers)[0]);
        VisitedNodes visitedNodesOutgoing(INTEGER(numberOfIdentifiers)[0]);

        contactChain(ingoing,
                     INTEGER(root)[i] - 1,
                     INTEGER(inBegin)[i],
                     INTEGER(inEnd)[i],
                     visitedNodesIngoing,
                     true);

        contactChain(outgoing,
                     INTEGER(root)[i] - 1,
                     INTEGER(outBegin)[i],
                     INTEGER(outEnd)[i],
                     visitedNodesOutgoing,
                     false);

        kv_push(int, ingoingContactChain, visitedNodesIngoing.N() - 1);
        kv_push(int, outgoingContactChain, visitedNodesOutgoing.N() - 1);

        kv_push(int, inDegree, degree(ingoing,
                                      INTEGER(root)[i] - 1,
                                      INTEGER(inBegin)[i],
                                      INTEGER(inEnd)[i]));

        kv_push(int, outDegree, degree(outgoing,
                                       INTEGER(root)[i] - 1,
                                       INTEGER(outBegin)[i],
                                       INTEGER(outEnd)[i]));
    }

    PROTECT(result = Rf_mkNamed(VECSXP, names));

    SET_VECTOR_ELT(result, 0, vec = Rf_allocVector(INTSXP, kv_size(inDegree)));
    memcpy(INTEGER(vec), &kv_A(inDegree, 0), kv_size(inDegree) * sizeof(int));

    SET_VECTOR_ELT(result, 1, vec = Rf_allocVector(INTSXP, kv_size(outDegree)));
    memcpy(INTEGER(vec), &kv_A(outDegree, 0), kv_size(outDegree) * sizeof(int));

    SET_VECTOR_ELT(result, 2, vec = Rf_allocVector(INTSXP, kv_size(ingoingContactChain)));
    memcpy(INTEGER(vec), &kv_A(ingoingContactChain, 0), kv_size(ingoingContactChain) * sizeof(int));

    SET_VECTOR_ELT(result, 3, vec = Rf_allocVector(INTSXP, kv_size(outgoingContactChain)));
    memcpy(INTEGER(vec), &kv_A(outgoingContactChain, 0), kv_size(outgoingContactChain) * sizeof(int));

cleanup:
    kv_destroy(ingoingContactChain);
    kv_destroy(outgoingContactChain);
    kv_destroy(inDegree);
    kv_destroy(outDegree);

    if (nprotect)
        UNPROTECT(nprotect);

    if (error)
        Rf_error("Unable to calculate network summary");

    return result;
}

static const R_CallMethodDef callMethods[] =
{
    {"networkSummary", (DL_FUNC) &networkSummary, 9},
    {"shortestPaths", (DL_FUNC) &shortestPaths, 9},
    {"traceContacts", (DL_FUNC) &traceContacts, 10},
    {NULL, NULL, 0}
};

/** Register routines to R
 * @param info Information about the DLL being loaded
 */
void attribute_visible
R_init_EpiContactTrace(DllInfo *info)
{
    R_registerRoutines(info, NULL, callMethods, NULL, NULL);
    R_useDynamicSymbols(info, FALSE);
    R_forceSymbols(info, TRUE);
}
