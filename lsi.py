import codecs
from gensim import corpora, models, similarities

print 'open corpora'
corpus = corpora.MmCorpus('bow.mm')
print 'open dictionary'
dictionary = corpora.Dictionary.load_from_text('wordid.txt')
print  'generate tfidf'
tfidf = models.TfidfModel(corpus, id2word=dictionary, normalize=True)
corpora.MmCorpus.serialize('tfidf.mm', tfidf[corpus], progress_cnt=10000)
corpus = corpora.MmCorpus('tfidf.mm')
print 'generate lsi'
lsi = models.LsiModel(corpus, id2word=dictionary, num_topics=100)
index = similarities.MatrixSimilarity(lsi[corpus])
lsi.save('irlsi.lsi')
index.save('irlsi.index')
