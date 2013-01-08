import codecs
from gensim import corpora, models

import logging
logging.basicConfig(format='%(asctime)s : %(levelname)s : %(message)s', level=logging.INFO)

print 'open corpora'
corpus = corpora.MmCorpus('bow.mm')
print 'open dictionary'
dictionary = corpora.Dictionary.load_from_text('wordid.txt')
print 'generate tfidf'
tfidf = models.TfidfModel(corpus, id2word=dictionary, normalize=True)
corpora.MmCorpus.serialize('tfidf.mm', tfidf[corpus], progress_cnt=10000)
tfidf.save('irlsi.tfidf')
