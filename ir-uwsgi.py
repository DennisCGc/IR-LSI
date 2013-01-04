from urlparse import urlparse,urlsplit,parse_qs
import re
import uwsgi
from gensim import corpora, models, similarities
from html import index as htmlindex
import simplejson
import codecs

HTML_HEADERS = [('Content-Type', 'text/html'), ('Access-Control-Allow-Origin', '*'), ('Access-Control-Allow-Headers','Requested-With,Content-Type')]
COMMON_HEADERS = [('Content-Type', 'text/plain'), ('Access-Control-Allow-Origin', '*'), ('Access-Control-Allow-Headers', 'Requested-With,Content-Type')]

docids = {}
f = codecs.open('docid.txt','r','UTF-8')
for line in f.read().split('\n')[:-1]:
    x = line.split('\t')
    docids[int(x[0])] = x[1]

def notfound(start_response):
    start_response('404 File Not Found', COMMON_HEADERS + [('Content-length', '2')])
    yield '[]'

print 'dictionary'
dictionary = corpora.Dictionary.load_from_text('wordid.txt')
print 'load corpus'
corpus = corpora.MmCorpus('tfidf.mm')
print 'load lsi'
lsi = models.LsiModel.load('irlsi.lsi')
print 'load index'
index = similarities.MatrixSimilarity.load('irlsi.index')

def LSIclient(environ, start_response):
    url = environ['PATH_INFO'][1:]
    arguments = url.split('/')
    if arguments[0] != 'api':
        reply = htmlindex
        start_response('200 OK', HTML_HEADERS + [('Content-length', str(len(reply)))])
        return reply        
    params = parse_qs(environ.get('QUERY_STRING',''))
    if 'query' not in params:
    	    return notfound(start_response)
    query = params['query'][0]
    print 'Querying %s' % query
    vec_bow = dictionary.doc2bow(query.lower().split())
    vec_lsi = lsi[vec_bow]
    sims = index[vec_lsi]
    sims = sorted(enumerate(sims), key=lambda item: -item[1])
    reply = []
    for doc in sims[:21]:
        docid = doc[0]
        if doc[0] in docids:
            docid = docids[docid]
        x = ( docid , str(doc[1]))
        reply.append(x)
    reply = simplejson.dumps(reply)
    start_response('200 OK', COMMON_HEADERS + [('Content-length', str(len(reply)))])
    return reply

uwsgi.applications = {'': LSIclient}
