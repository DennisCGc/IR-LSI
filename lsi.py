import codecs
from gensim import corpora, models, similarities, utils

import logging
logging.basicConfig(format='%(asctime)s : %(levelname)s : %(message)s', level=logging.INFO)

print 'open corpora'
corpus = corpora.MmCorpus('tfidf.mm')
print 'creating fake dictionary'
fakedict = utils.FakeDict(corpus.num_terms)
print 'generate lsi'
lsi = models.LsiModel(corpus=corpus, id2word=fakedict, num_topics=150)
lsi.save('irlsi.lsi')
print 'generate index'
index = similarities.Similarity(corpus=lsi[corpus], num_features = 150, output_prefix="shard")
index.save('irlsi.index')
